export module vrento:vulkan.pass.input;
export import :vulkan.graph;
export import :vulkan.pass.desc;
export import :pass.graph_builder;
import rstd;
using rstd::sync::Arc;

export namespace vrento::vulkan
{
class PassInput {
    struct DeclaredUse {
        GraphSerial id;
        PassUsage   usage;
        PassStages  stages;
    };
    struct Data {
        GraphSerial              scope, revision;
        PassExecutionInfo        execution;
        Vec<DeclaredUse>         declared_uses;
        Arc<dyn<pass::PassDesc>> description;
    };
    Arc<Data> data_;
    explicit PassInput(Arc<Data> data): data_(rstd::move(data)) {}

public:
    template<typename T>
    static auto publish(GraphSerial scope, GraphSerial revision, PassId id, T value,
                        fg::FrameGraphBuilder* builder = nullptr, String name = {})
        -> Result<PassInput, GraphError> {
        if (! scope || ! revision || ! id.value)
            return Err(GraphError { GraphErrorKind::Conflict, id });
        if constexpr (requires { value.validate(); }) {
            auto valid = value.validate();
            if (valid.is_err()) return Err(valid.unwrap_err());
        }
        auto uses  = describe_io(value);
        auto valid = uses.validate();
        if (valid.is_err()) return Err(valid.unwrap_err());
        auto             execution = uses.execution(id, ! builder);
        Vec<DeclaredUse> declared_uses;
        if (builder) {
            auto registered = pass::declare_io(
                *builder, id, { execution.queue.id }, uses.logical(), rstd::move(name));
            if (registered.is_err()) return Err(registered.unwrap_err());
            declared_uses.reserve(registered->len());
            auto append = [&](const VulkanResourceUse& use) {
                declared_uses.push({ (*registered)[declared_uses.len()], use.usage, use.stages });
            };
            for (const auto& input : uses.inputs) append(input);
            for (const auto& output : uses.outputs) append(output);
        } else {
            for (const auto& input : uses.inputs)
                if (auto source = input.content.resolve(scope); source.is_err())
                    return Err(source.unwrap_err());
            for (const auto& output : uses.outputs) {
                if (auto target = output.content.resolve(scope); target.is_err())
                    return Err(target.unwrap_err());
                if (auto reference = output.content.output();
                    reference && reference->producer != id)
                    return Err(GraphError { GraphErrorKind::Conflict, id });
            }
        }
        return Ok(
            PassInput(Arc<Data>::make(Data { scope,
                                             revision,
                                             rstd::move(execution),
                                             rstd::move(declared_uses),
                                             Arc<dyn<pass::PassDesc>>::make(rstd::move(value)) })));
    }
    auto scope() const -> GraphSerial { return data_->scope; }
    auto revision() const -> GraphSerial { return data_->revision; }
    auto execution() const -> const PassExecutionInfo& { return data_->execution; }
    auto lower(const fg::FrameGraph& graph) const -> Result<PassExecutionInfo, GraphError> {
        auto result = execution().clone();
        if (data_->declared_uses.is_empty()) return Ok(rstd::move(result));
        auto node = graph.find_pass(result.id);
        if (! node || node->uses.len() != data_->declared_uses.len())
            return Err(GraphError { GraphErrorKind::Conflict, result.id });
        result.uses.reserve(node->uses.len());
        for (usize i {}; i < node->uses.len(); ++i) {
            const auto& source  = node->uses[i];
            const auto& details = data_->declared_uses[i];
            result.uses.push({ source.version.resource,
                               source.version.version,
                               source.access,
                               details.usage,
                               details.stages,
                               source.range,
                               details.id });
        }
        return Ok(rstd::move(result));
    }
    auto type() const -> rstd::any::TypeId { return description().concrete_type_id(); }
    template<typename T>
    auto get() const -> Option<ref<T>> {
        return pass::try_desc<T>(description());
    }
    auto description() const -> ref<dyn<pass::PassDesc>> {
        return data_->description.as_ptr().as_ptr().as_ref();
    }
    auto io() const -> pass::PassIO { return description()->IO(); }
    auto same_input(const PassInput& other) const -> bool {
        return Arc<Data>::ptr_eq(data_, other.data_);
    }
    auto clone() const -> PassInput { return PassInput(data_.clone()); }
};

struct DeclaredContentPass {
    PassInput        description;
    pass::ContentRef output;
};
inline auto declare_input(fg::FrameGraphBuilder& graph, const PassInput& input, String name = {})
    -> Result<empty, GraphError> {
    if (graph.data().scope != input.scope() || ! input.revision())
        return Err(GraphError { GraphErrorKind::Conflict, input.execution().id });
    auto declared = pass::declare_io(
        graph, input.execution().id, { input.execution().queue.id }, input.io(), rstd::move(name));
    if (declared.is_err()) return Err(declared.unwrap_err());
    return Ok(empty {});
}
template<typename T>
inline auto declare_input(fg::FrameGraphBuilder& graph, PassId id, T description,
                          GraphSerial revision = 1, String name = {})
    -> Result<PassInput, GraphError> {
    return PassInput::publish(
        graph.data().scope, revision, id, rstd::move(description), &graph, rstd::move(name));
}

} // namespace vrento::vulkan
