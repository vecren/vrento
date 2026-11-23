export module vrento:vulkan.renderer;
export import :vulkan.pass.compiled;
export import :vulkan.pass.factory;
import rstd;

using rstd::sync::Arc;

export namespace vrento::vulkan
{
struct RendererStatistics {
    GraphSerial graph_compiles {}, native_compiles {}, pending_polls {};
    GraphSerial compile_requests {}, graph_reuses {}, native_completed {}, compile_failures {},
        compile_cancellations {};
    ExecutionStatistics execution;
};
enum class CompileReason
{
    Initial,
    Reused,
    Scope,
    Graph,
    Description,
    Resources,
    Standalone
};
enum class CompileState
{
    Idle,
    Pending,
    Ready,
    Failed,
    Cancelled
};
enum class CompileKind
{
    Graph,
    Pass
};
struct CompileStatus {
    GraphSerial                         id {};
    CompileState                        state { CompileState::Idle };
    Option<GraphExecutionErrorSnapshot> error;
    CompileKind                         kind { CompileKind::Graph };
    GraphSerial                         scope {};
    CompileReason                       reason { CompileReason::Initial };
};
class Renderer {
    GraphSerial            next_compile_id_ {};
    CompileStatus          status_;
    DeviceSession          session_;
    RendererStatistics     statistics_;
    Arc<ExecutionCounters> execution_ { Arc<ExecutionCounters>::make() };
    struct FactoryEntry {
        rstd::any::TypeId     type;
        Arc<dyn<PassFactory>> factory;
    };
    Vec<FactoryEntry> factories_;
    struct PendingCompile {
        Option<VulkanPassSet> graph;
        Option<GraphInput>    input;
        Vec<PassInput>        standalone;
        auto                  descriptions() const -> slice<PassInput> {
            return input ? input->descriptions() : standalone.as_slice();
        }
        Vec<CompiledGraph::Entry>             entries;
        Option<Arc<GraphicsCompileResources>> resources;
        Option<PassExecutionInfo>             standalone_pass;
        Option<GraphBindingLayout>            standalone_layout;
    };
    Option<PendingCompile> pending_;
    Option<CompiledGraph>  compiled_;
    Option<CompiledPass>   compiled_pass_;

public:
    explicit Renderer(DeviceSession session): session_(rstd::move(session)) {}
    auto session() const -> const DeviceSession& { return session_; }
    auto statistics() const -> RendererStatistics {
        auto value      = statistics_;
        value.execution = execution_->snapshot();
        return value;
    }
    auto bind(Arc<dyn<PassFactory>> factory) -> Result<empty, GraphExecutionError> {
        auto type = factory->Type();
        for (const auto& entry : factories_)
            if (entry.type == type) return Err(GraphExecutionError { GraphExecutionStage::Schema });
        factories_.push({ type, rstd::move(factory) });
        return Ok(empty {});
    }
    auto status() const -> CompileStatus {
        return { status_.id,   status_.state, status_.error.clone(),
                 status_.kind, status_.scope, status_.reason };
    }
    auto ready() const -> bool {
        return session_.valid() && ! session_.lost() && status_.state == CompileState::Ready &&
               (status_.kind == CompileKind::Graph ? compiled_.is_some()
                                                   : compiled_pass_.is_some());
    }
    auto compile(GraphInput input, Option<Arc<GraphicsCompileResources>> resources = None())
        -> Result<CompileProgress, GraphExecutionError> {
        cancel_compile();
        ++statistics_.compile_requests;
        status_ = { ++next_compile_id_,
                    CompileState::Pending,
                    None(),
                    CompileKind::Graph,
                    input.graph().scope() };
        return finish_compile(begin_compile(rstd::move(input), rstd::move(resources)));
    }
    auto compile(fg::FrameGraph graph, Vec<PassInput> descriptions,
                 Option<Arc<GraphicsCompileResources>> resources = None())
        -> Result<CompileProgress, GraphExecutionError> {
        pass::GraphResourceMap mapping { .scope = graph.scope() };
        const auto&            plan = graph.inspect();
        for (usize i {}; i < plan.accesses.len(); ++i)
            if (plan.accesses[i].used())
                mapping.externals.push({ plan.lifetimes[i].version, fg::Index(i.to_primitive()) });
        auto input =
            GraphInput::create(rstd::move(graph), rstd::move(descriptions), rstd::move(mapping));
        if (input.is_err()) {
            cancel_compile();
            ++statistics_.compile_requests;
            status_ = { ++next_compile_id_, CompileState::Pending, None(), CompileKind::Graph };
            return finish_compile(Err(GraphExecutionError {
                GraphExecutionStage::Schema,
                input.unwrap_err().pass,
                input.unwrap_err().resource,
                vrento::vulkan::GraphExecutionCause::Graph(input.unwrap_err()) }));
        }
        return compile(input.unwrap(), rstd::move(resources));
    }
    auto continue_compile(GraphSerial id) -> Result<CompileProgress, GraphExecutionError> {
        if (id != status_.id || status_.state != CompileState::Pending || ! pending_)
            return Err(GraphExecutionError {
                GraphExecutionStage::Prepare,
                {},
                {},
                GraphExecutionCause::Reason(GraphFailure::StaleTransaction) });
        return finish_compile(advance_compile());
    }

private:
    auto finish_compile(Result<CompileProgress, GraphExecutionError> result)
        -> Result<CompileProgress, GraphExecutionError> {
        if (result.is_ok()) {
            status_.state =
                *result == CompileProgress::Ready ? CompileState::Ready : CompileState::Pending;
            status_.error = None();
        } else {
            const auto& error = result.as_ref().unwrap_err();
            status_.state     = CompileState::Failed;
            status_.error     = Some(GraphExecutionErrorSnapshot::capture(error));
            if (status_.state == CompileState::Failed) {
                ++statistics_.compile_failures;
                pending_ = None();
            }
        }
        return result;
    }
    auto begin_compile(GraphInput input, Option<Arc<GraphicsCompileResources>> resources = None())
        -> Result<CompileProgress, GraphExecutionError> {
        auto graph        = input.graph();
        auto descriptions = input.descriptions();
        if (! session_.valid() || session_.lost())
            return Err(GraphExecutionError {
                GraphExecutionStage::Device,
                {},
                {},
                session_.lost() ? GraphExecutionCause::Native(VK_ERROR_DEVICE_LOST)
                                : GraphExecutionCause::Reason(GraphFailure::Closed) });
        if (compiled_) {
            const auto& previous = *compiled_->data_;
            if (graph.scope() != previous.graph.scope())
                status_.reason = CompileReason::Scope;
            else {
                auto current = fg::encode_frame_graph(graph);
                auto cached  = fg::encode_frame_graph(previous.graph.frame_graph());
                if (current.is_err() || cached.is_err() || *current != *cached)
                    status_.reason = CompileReason::Graph;
                else {
                    bool same = descriptions.len() == previous.input.descriptions().len();
                    for (usize i {}; same && i < descriptions.len(); ++i)
                        same = descriptions[i].same_input(previous.input.descriptions()[i]);
                    if (! same)
                        status_.reason = CompileReason::Description;
                    else if (! input.resources().same_bindings(previous.input.resources()) ||
                             resources.is_some() != previous.resources.is_some() ||
                             (resources && ! Arc<GraphicsCompileResources>::ptr_eq(
                                               *resources, *previous.resources)))
                        status_.reason = CompileReason::Resources;
                    else {
                        status_.reason = CompileReason::Reused;
                        ++statistics_.graph_reuses;
                        return Ok(CompileProgress::Ready);
                    }
                }
            }
        }
        ++statistics_.graph_compiles;
        Vec<PassExecutionInfo> declarations;
        for (const auto& desc : descriptions) {
            if (desc.scope() != graph.scope() || ! desc.revision())
                return Err(
                    GraphExecutionError { GraphExecutionStage::Schema, desc.execution().id });
            auto lowered = desc.lower(graph);
            if (lowered.is_err()) {
                auto error = lowered.unwrap_err();
                return Err(GraphExecutionError { GraphExecutionStage::Graph,
                                                 error.pass,
                                                 error.resource,
                                                 GraphExecutionCause::Graph(error) });
            }
            declarations.push(lowered.unwrap());
        }
        auto set = VulkanPassSet::associate(rstd::move(graph), rstd::move(declarations));
        if (set.is_err())
            return Err(GraphExecutionError {
                GraphExecutionStage::Graph,
                set.unwrap_err().pass,
                set.unwrap_err().resource,
                vrento::vulkan::GraphExecutionCause::Graph(set.unwrap_err()) });
        pending_ = Some(PendingCompile {
            Some(set.unwrap()), Some(rstd::move(input)), {}, {}, rstd::move(resources) });
        return advance_compile();
    }

public:
    auto compile_pass(PassInput                             description,
                      Option<Arc<GraphicsCompileResources>> resources = None())
        -> Result<Preparation<CompiledPass>, GraphExecutionError> {
        cancel_compile();
        ++statistics_.compile_requests;
        status_ = { ++next_compile_id_, CompileState::Pending, None(),
                    CompileKind::Pass,  description.scope(),   CompileReason::Standalone };
        if (! session_.valid() || session_.lost()) {
            auto failed = finish_compile(Err(GraphExecutionError {
                GraphExecutionStage::Device,
                {},
                {},
                session_.lost() ? GraphExecutionCause::Native(VK_ERROR_DEVICE_LOST)
                                : GraphExecutionCause::Reason(GraphFailure::Closed) }));
            return Err(failed.unwrap_err());
        }
        if (! description.scope() || ! description.revision()) {
            auto failed = finish_compile(Err(
                GraphExecutionError { GraphExecutionStage::Schema, description.execution().id }));
            return Err(failed.unwrap_err());
        }
        auto           pass   = description.execution().clone();
        auto           layout = GraphBindingLayout::for_pass(description.scope(), pass);
        Vec<PassInput> descriptions;
        descriptions.push(rstd::move(description));
        pending_    = Some(PendingCompile { None(),
                                            None(),
                                            rstd::move(descriptions),
                                            {},
                                            rstd::move(resources),
                                            Some(rstd::move(pass)),
                                            Some(rstd::move(layout)) });
        auto result = finish_compile(advance_compile());
        if (result.is_err()) return Err(result.unwrap_err());
        if (*result == CompileProgress::Pending) return Ok(PendingPreparation {});
        return Ok(CompiledPass(*compiled_pass_));
    }

private:
    auto advance_compile() -> Result<CompileProgress, GraphExecutionError> {
        if (! pending_) return Err(GraphExecutionError { GraphExecutionStage::Prepare });
        if (session_.lost()) {
            pending_ = None();
            return Err(GraphExecutionError {
                GraphExecutionStage::Device,
                {},
                {},
                vrento::vulkan::GraphExecutionCause::Native(VK_ERROR_DEVICE_LOST) });
        }
        ++statistics_.pending_polls;
        auto&              pending = *pending_;
        PassCompileContext context { session_, pending.resources.clone() };
        for (usize i(pending.entries.len()); i < pending.descriptions().len(); ++i) {
            const auto&                   desc = pending.descriptions()[i];
            Option<Arc<dyn<VulkanPass>>>  native;
            Option<Arc<dyn<PassFactory>>> factory;
            for (auto& entry : factories_)
                if (entry.type == desc.type()) {
                    ++statistics_.native_compiles;
                    const auto& pass =
                        pending.graph
                            ? pending.graph->passes()[usize(
                                  pending.graph->frame_graph().pass_slot(desc.execution().id))]
                            : *pending.standalone_pass;
                    auto result = entry.factory->Compile(desc, pass, context);
                    if (result.is_err()) {
                        auto error = result.unwrap_err();
                        error.pass = desc.execution().id;
                        pending_   = None();
                        return Err(rstd::move(error));
                    }
                    if (! result->ready()) return Ok(CompileProgress::Pending);
                    auto completed = result->take_ready();
                    ++statistics_.native_completed;
                    if (completed.as_ptr().as_ptr().as_ref().concrete_type_id() !=
                        entry.factory->PassType()) {
                        auto id  = desc.execution().id;
                        pending_ = None();
                        return Err(GraphExecutionError { GraphExecutionStage::Schema, id });
                    }
                    factory = Some(entry.factory.clone());
                    native  = Some(rstd::move(completed));
                    break;
                }
            if (! native) {
                auto id  = desc.execution().id;
                pending_ = None();
                return Err(GraphExecutionError { GraphExecutionStage::Schema, id });
            }
            pending.entries.push({ desc.execution().id,
                                   desc.revision(),
                                   rstd::move(*native),
                                   rstd::move(*factory) });
        }
        if (pending.graph) {
            auto result = CompiledGraph::publish(rstd::move(*pending.graph),
                                                 session_,
                                                 rstd::move(*pending.input),
                                                 rstd::move(pending.entries),
                                                 pending.resources.clone(),
                                                 execution_.clone());
            if (result.is_err()) return Err(result.unwrap_err());
            compiled_ = Some(result.unwrap());
        } else {
            compiled_pass_.insert(CompiledPass(
                Arc<CompiledPass::Data>::make(session_,
                                              rstd::move(pending.standalone[usize()]),
                                              pending.standalone_pass.take().unwrap(),
                                              pending.standalone_layout.take().unwrap(),
                                              pending.entries[usize()].native.clone(),
                                              pending.entries[usize()].factory.clone(),
                                              pending.resources.clone(),
                                              execution_.clone())));
        }
        pending_ = None();
        return Ok(CompileProgress::Ready);
    }

public:
    auto pending() const -> bool { return status_.state == CompileState::Pending; }
    auto compiled() const -> Option<CompiledGraph> {
        return compiled_ ? Some(CompiledGraph(*compiled_)) : None<CompiledGraph>();
    }
    auto compiled_pass() const -> Option<CompiledPass> {
        return compiled_pass_ ? Some(CompiledPass(*compiled_pass_)) : None<CompiledPass>();
    }
    auto cancel_compile(GraphSerial id) -> bool {
        if (id != status_.id || ! pending()) return false;
        pending_ = None();
        ++statistics_.compile_cancellations;
        status_.state = CompileState::Cancelled;
        status_.error = None();
        return true;
    }
    void cancel_compile() { (void)cancel_compile(status_.id); }
    void clear() {
        cancel_compile();
        status_        = {};
        pending_       = None();
        compiled_      = None();
        compiled_pass_ = None();
    }
};

} // namespace vrento::vulkan
