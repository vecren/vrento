export module vrento:vulkan.pass.execution;
export import :frame_graph;
import rstd;

using namespace rstd::prelude;
using rstd::collections::HashMap;

export namespace vrento::vulkan
{
using GraphIndex  = rstd::uint32_t;
using GraphSerial = rstd::uint64_t;

using ResourceId        = fg::ResourceId;
using PassId            = fg::PassId;
using GraphResourceKind = fg::ResourceKind;
using GraphAccess       = fg::Access;
using GraphRange        = fg::ResourceRange;
enum class QueueClass : GraphIndex
{
    Graphics,
    Compute,
    Transfer
};
struct LogicalQueue {
    GraphIndex  id {};
    QueueClass  capability { QueueClass::Graphics };
    friend bool operator==(const LogicalQueue&, const LogicalQueue&) = default;
};
enum class PassUsage : GraphIndex
{
    Sampled,
    Storage,
    ColorAttachment,
    DepthAttachment,
    Transfer,
    Vertex,
    Index,
    Indirect,
    Uniform,
    Release
};
enum class PassStages : GraphIndex
{
    Vertex   = 1,
    Fragment = 2,
    Compute  = 4,
    Transfer = 8,
    Color    = 16,
    Depth    = 32,
    Indirect = 64,
    Geometry = 128
};
constexpr auto operator|(PassStages a, PassStages b) -> PassStages {
    return PassStages(GraphIndex(a) | GraphIndex(b));
}
struct GraphBindingRequirement {
    ResourceId        id;
    GraphResourceKind kind { GraphResourceKind::Texture };
    GraphSerial       bytes {};
    GraphIndex        mip_levels { 1 }, layers { 1 }, aspects { 1 };
    bool              imported {};
    GraphSerial       content_revision {};
    bool              check_revision {};
    bool              fixed_storage { true };
    bool              read_only_import {};
};
struct GraphBindingSlot {
    fg::Index   index { fg::invalid_index };
    auto        valid() const -> bool { return index != fg::invalid_index; }
    friend bool operator==(GraphBindingSlot, GraphBindingSlot) = default;
};
struct PassResourceUse {
    ResourceId  resource;
    GraphIndex  version {};
    GraphAccess access { GraphAccess::Read };
    PassUsage   usage { PassUsage::Sampled };
    PassStages  stages { PassStages::Fragment };
    GraphRange  range;
    GraphSerial logical_use {};
    GraphBindingSlot binding;
    friend bool operator==(const PassResourceUse&, const PassResourceUse&) = default;
};
enum class AttachmentLoad : GraphIndex
{
    Load,
    Clear,
    Discard
};
enum class AttachmentStore : GraphIndex
{
    Store,
    Discard
};
struct GraphAttachment {
    ResourceId            resource;
    GraphIndex            version { 1 };
    AttachmentLoad        load { AttachmentLoad::Clear };
    AttachmentStore       store { AttachmentStore::Store };
    rstd::array<float, 4> clear {};
    GraphIndex            stencil {};
    GraphBindingSlot      binding;
    friend bool           operator==(const GraphAttachment&, const GraphAttachment&) = default;
};
struct GraphicsDescription {
    Option<GraphAttachment> color, depth, resolve;
    GraphIndex              x {}, y {}, width {}, height {};
    bool                    full_target { true };
    friend bool operator==(const GraphicsDescription&, const GraphicsDescription&) = default;
};
struct PassExecutionInfo {
    PassId                      id;
    GraphSerial                 kind {};
    GraphIndex                  schema_version { 1 };
    LogicalQueue                queue;
    Vec<rstd::uint8_t>          parameters;
    Vec<PassResourceUse>        uses;
    Option<GraphicsDescription> graphics;
    friend bool operator==(const PassExecutionInfo&, const PassExecutionInfo&) = default;
    auto        clone() const -> PassExecutionInfo {
        return {
            id, kind, schema_version, queue, parameters.clone(), uses.clone(), graphics.clone()
        };
    }
};
using GraphDependency                             = fg::Dependency;
inline constexpr GraphSerial texture_copy_kind    = 0x7672742e74636f70ULL;
inline constexpr GraphSerial texture_mips_kind    = 0x7672742e6d697073ULL;
inline constexpr GraphSerial texture_sampled_kind = 0x7672742e74726561ULL;
struct VulkanPassData {
    GraphSerial                  scope {};
    Vec<GraphBindingRequirement> resources;
    Vec<PassExecutionInfo>       passes;
    Vec<GraphDependency>         dependencies;
};
struct GraphLimits {
    GraphIndex  resources { 16384 }, passes { 4096 }, uses { 65536 }, dependencies { 262144 };
    GraphSerial parameter_bytes { 16 * 1024 * 1024 }, encoded_bytes { 32 * 1024 * 1024 };
};
using GraphError         = fg::Error;
using GraphErrorKind     = fg::ErrorKind;
using GraphExecutionPlan = fg::FrameGraphPlan;
} // namespace vrento::vulkan
export namespace vrento::vulkan
{
inline auto validate_use(const GraphBindingRequirement& resource, const PassResourceUse& use,
                         LogicalQueue queue) -> Result<empty, GraphError> {
    auto error = [&](GraphErrorKind kind) {
        return Err(GraphError { kind, {}, use.resource });
    };
    if (GraphIndex(use.access) > GraphIndex(GraphAccess::Write) ||
        GraphIndex(use.usage) > GraphIndex(PassUsage::Release) || ! GraphIndex(use.stages) ||
        (GraphIndex(use.stages) & ~GraphIndex(255)))
        return error(GraphErrorKind::Conflict);
    if (queue.capability == QueueClass::Transfer &&
        ((use.usage != PassUsage::Transfer && use.usage != PassUsage::Release) ||
         use.stages != PassStages::Transfer))
        return error(GraphErrorKind::Conflict);
    if (queue.capability == QueueClass::Compute &&
        (GraphIndex(use.stages) &
         ~GraphIndex(PassStages::Compute | PassStages::Transfer | PassStages::Indirect)))
        return error(GraphErrorKind::Conflict);
    if (resource.kind == GraphResourceKind::Buffer) {
        if (! use.range.size || use.range.offset > resource.bytes ||
            use.range.size > resource.bytes - use.range.offset)
            return error(GraphErrorKind::Range);
        if (use.usage == PassUsage::Sampled || use.usage == PassUsage::ColorAttachment ||
            use.usage == PassUsage::DepthAttachment)
            return error(GraphErrorKind::Conflict);
    } else {
        const auto& range = use.range;
        if (range.offset || range.size || ! range.aspect || (range.aspect & ~resource.aspects) ||
            ! range.mip_count || ! range.layer_count || range.first_mip >= resource.mip_levels ||
            range.mip_count > resource.mip_levels - range.first_mip ||
            range.first_layer >= resource.layers ||
            range.layer_count > resource.layers - range.first_layer)
            return error(GraphErrorKind::Range);
        if (use.usage == PassUsage::Vertex || use.usage == PassUsage::Index ||
            use.usage == PassUsage::Indirect || use.usage == PassUsage::Uniform)
            return error(GraphErrorKind::Conflict);
    }
    if (use.access == GraphAccess::Write &&
        (use.version == 0 || use.usage == PassUsage::Sampled || use.usage == PassUsage::Vertex ||
         use.usage == PassUsage::Index || use.usage == PassUsage::Indirect ||
         use.usage == PassUsage::Uniform || use.usage == PassUsage::Release))
        return error(GraphErrorKind::Conflict);
    GraphIndex allowed {};
    switch (use.usage) {
    case PassUsage::Release: allowed = 255; break;
    case PassUsage::Transfer: allowed = GraphIndex(PassStages::Transfer); break;
    case PassUsage::ColorAttachment: allowed = GraphIndex(PassStages::Color); break;
    case PassUsage::DepthAttachment: allowed = GraphIndex(PassStages::Depth); break;
    case PassUsage::Vertex:
    case PassUsage::Index: allowed = GraphIndex(PassStages::Vertex); break;
    case PassUsage::Indirect: allowed = GraphIndex(PassStages::Indirect); break;
    default:
        allowed = GraphIndex(PassStages::Vertex | PassStages::Fragment | PassStages::Compute |
                             PassStages::Geometry);
        break;
    }
    if (GraphIndex(use.stages) & ~allowed) return error(GraphErrorKind::Conflict);
    return Ok(empty {});
}

template<typename ResourceAt, typename SlotAt>
inline auto check_pass_execution(const PassExecutionInfo& pass, GraphLimits limits,
                                 ResourceAt resource_at, SlotAt slot_at)
    -> Result<empty, GraphError> {
    if (! pass.id.valid() || ! pass.kind || ! pass.schema_version ||
        GraphIndex(pass.queue.capability) > GraphIndex(QueueClass::Transfer))
        return Err(GraphError { GraphErrorKind::Invalid, pass.id });
    if (pass.uses.len().to_primitive() > limits.uses ||
        pass.parameters.len().to_primitive() > limits.parameter_bytes)
        return Err(GraphError { GraphErrorKind::Limit, pass.id });
    for (usize u {}; u < pass.uses.len(); ++u) {
        const auto& use   = pass.uses[u];
        auto        found = resource_at(use);
        if (! found)
            return Err(GraphError { GraphErrorKind::MissingResource, pass.id, use.resource });
        auto checked = validate_use(*found, use, pass.queue);
        if (checked.is_err()) {
            auto error = checked.unwrap_err();
            error.pass = pass.id;
            return Err(error);
        }
        for (usize v {}; v < u; ++v) {
            const auto& previous = pass.uses[v];
            if (previous.resource != use.resource ||
                ! fg::overlaps(found->kind, previous.range, use.range) ||
                (previous.access == GraphAccess::Read && use.access == GraphAccess::Read))
                continue;
            if (previous.version != use.version) {
                auto previous_slot = slot_at({ previous.resource, previous.version });
                auto current_slot  = slot_at({ use.resource, use.version });
                if (previous_slot != fg::invalid_index && current_slot != fg::invalid_index &&
                    previous_slot != current_slot)
                    continue;
            }
            if (previous.access != use.access && previous.usage == use.usage &&
                (use.usage == PassUsage::Storage || use.usage == PassUsage::ColorAttachment ||
                 use.usage == PassUsage::DepthAttachment ||
                 (use.usage == PassUsage::Transfer && ! found->fixed_storage))) {
                auto read  = previous.access == GraphAccess::Read ? previous.version : use.version;
                auto write = previous.access == GraphAccess::Write ? previous.version : use.version;
                if (write && read == write - 1) continue;
            }
            return Err(GraphError { GraphErrorKind::Conflict, pass.id, use.resource });
        }
    }
    if (pass.graphics.is_some()) {
        const auto& graphics = *pass.graphics;
        if (pass.queue.capability != QueueClass::Graphics ||
            (! graphics.color && ! graphics.depth) || (graphics.resolve && ! graphics.color) ||
            (graphics.full_target &&
             (graphics.x || graphics.y || graphics.width || graphics.height)) ||
            (! graphics.full_target && (! graphics.width || ! graphics.height)))
            return Err(GraphError { GraphErrorKind::Invalid, pass.id });
        auto attachment = [&](const Option<GraphAttachment>& value,
                              PassUsage                      usage) -> Result<empty, GraphError> {
            if (value.is_none()) return Ok(empty {});
            const auto& target = *value;
            if (GraphIndex(target.load) > GraphIndex(AttachmentLoad::Discard) ||
                GraphIndex(target.store) > GraphIndex(AttachmentStore::Discard))
                return Err(GraphError { GraphErrorKind::Invalid, pass.id, target.resource });
            for (auto channel : target.clear)
                if ((__builtin_bit_cast(GraphIndex, channel) & 0x7f800000u) == 0x7f800000u)
                    return Err(GraphError { GraphErrorKind::Invalid, pass.id, target.resource });
            if (usage == PassUsage::DepthAttachment && target.load == AttachmentLoad::Clear &&
                (target.clear[usize()] < 0 || target.clear[usize()] > 1))
                return Err(GraphError { GraphErrorKind::Invalid, pass.id, target.resource });
            bool write {}, load = target.load != AttachmentLoad::Load;
            for (const auto& use : pass.uses) {
                if (use.resource != target.resource || use.usage != usage) continue;
                write |= use.access == GraphAccess::Write && use.version == target.version;
                load |= use.access == GraphAccess::Read && target.version &&
                        use.version == target.version - 1;
            }
            if (! write || ! load)
                return Err(GraphError { GraphErrorKind::Conflict, pass.id, target.resource });
            return Ok(empty {});
        };
        for (unsigned role {}; role < 3; ++role) {
            auto valid = role == 0   ? attachment(graphics.color, PassUsage::ColorAttachment)
                         : role == 1 ? attachment(graphics.depth, PassUsage::DepthAttachment)
                                     : attachment(graphics.resolve, PassUsage::ColorAttachment);
            if (valid.is_err()) return Err(valid.unwrap_err());
        }
        if (graphics.resolve && graphics.resolve->load != AttachmentLoad::Discard)
            return Err(
                GraphError { GraphErrorKind::Conflict, pass.id, graphics.resolve->resource });
    }

    return Ok(empty {});
}

inline auto
validate_pass_execution(const PassExecutionInfo& pass, slice<GraphBindingRequirement> resources,
                        GraphLimits limits = {}, slice<fg::AllocationSlot> allocations = {})
    -> Result<empty, GraphError> {
    return check_pass_execution(
        pass,
        limits,
        [&](const PassResourceUse& use) -> const GraphBindingRequirement* {
            for (const auto& resource : resources)
                if (resource.id == use.resource) return rstd::addressof(resource);
            return nullptr;
        },
        [&](fg::ResourceVersion id) -> fg::Index {
            for (auto allocation : allocations)
                if (allocation.version == id) return allocation.slot;
            return fg::invalid_index;
        });
}
struct GraphBindingVersion {
    fg::ResourceVersion version;
    fg::Index           resource { fg::invalid_index }, allocation { fg::invalid_index };
    fg::Index           first_slotted { fg::invalid_index };
    PassId              first_pass;
    bool                used {}, written {};
};
class GraphBindingLayout {
    struct Data {
        GraphSerial              scope;
        Vec<GraphBindingVersion> versions;
    };
    rstd::sync::Arc<Data> data_;
    explicit GraphBindingLayout(Data data): data_(rstd::sync::Arc<Data>::make(rstd::move(data))) {}

public:
    GraphBindingLayout(const GraphBindingLayout& other): data_(other.data_.clone()) {}
    GraphBindingLayout(GraphBindingLayout&&) = default;
    auto scope() const -> GraphSerial { return data_->scope; }
    auto versions() const -> slice<GraphBindingVersion> { return data_->versions.as_slice(); }
    bool same_layout(const GraphBindingLayout& other) const {
        return rstd::sync::Arc<Data>::ptr_eq(data_, other.data_);
    }
    static auto from_graph(const fg::FrameGraph& graph) -> GraphBindingLayout {
        const auto&    plan = graph.inspect();
        Data           data { .scope = graph.scope() };
        Vec<fg::Index> first_slots;
        first_slots.resize(graph.resources().len(), fg::invalid_index);
        data.versions.reserve(graph.versions().len());
        for (usize i {}; i < graph.versions().len(); ++i) {
            auto      version       = graph.versions()[i].id;
            auto      first         = plan.lifetimes[i].uses.first();
            auto      resource      = graph.resource_slot(version.resource);
            auto      allocation    = plan.allocations[i].slot;
            fg::Index first_slotted = fg::invalid_index;
            if (allocation != fg::invalid_index && plan.accesses[i].used()) {
                auto& slot    = first_slots[usize(resource)];
                first_slotted = slot;
                if (slot == fg::invalid_index) slot = fg::Index(i.to_primitive());
            }
            data.versions.push({ version,
                                 resource,
                                 allocation,
                                 first_slotted,
                                 first ? graph.passes()[usize(**first)].id : PassId {},
                                 plan.accesses[i].used(),
                                 plan.accesses[i].written });
        }
        return GraphBindingLayout(rstd::move(data));
    }
    static auto for_pass(GraphSerial scope, PassExecutionInfo& pass) -> GraphBindingLayout {
        Data                                           data { .scope = scope };
        HashMap<fg::ResourceVersion, GraphBindingSlot> lookup;
        for (auto& use : pass.uses) {
            fg::ResourceVersion version { use.resource, use.version };
            auto                found = lookup.get(version);
            if (found) {
                use.binding = **found;
            } else {
                use.binding = { fg::Index(data.versions.len().to_primitive()) };
                (void)lookup.insert(version, use.binding);
                data.versions.push({ .version = version, .first_pass = pass.id, .used = true });
            }
            data.versions[usize(use.binding.index)].written |= use.access == GraphAccess::Write;
        }
        if (pass.graphics) {
            Option<GraphAttachment>* attachments[] { &pass.graphics->color,
                                                     &pass.graphics->depth,
                                                     &pass.graphics->resolve };
            for (auto* attachment : attachments) {
                if (! *attachment) continue;
                auto& value   = **attachment;
                auto  found   = lookup.get({ value.resource, value.version });
                value.binding = found ? **found : GraphBindingSlot {};
            }
        }
        return GraphBindingLayout(rstd::move(data));
    }
};
class VulkanPassSet {
    struct Data {
        VulkanPassData     declarations;
        fg::FrameGraph     graph;
        GraphBindingLayout bindings;
        Data(VulkanPassData values, fg::FrameGraph compiled)
            : declarations(rstd::move(values)),
              graph(rstd::move(compiled)),
              bindings(GraphBindingLayout::from_graph(graph)) {}
    };
    rstd::sync::Arc<Data> data_;
    VulkanPassSet(VulkanPassData declarations, fg::FrameGraph graph)
        : data_(rstd::sync::Arc<Data>::make(rstd::move(declarations), rstd::move(graph))) {}

public:
    VulkanPassSet(const VulkanPassSet& other): data_(other.data_.clone()) {}
    VulkanPassSet(VulkanPassSet&&)                    = default;
    auto operator=(VulkanPassSet&&) -> VulkanPassSet& = default;
    auto operator=(const VulkanPassSet& other) -> VulkanPassSet& {
        if (this != &other) data_ = other.data_.clone();
        return *this;
    }
    static auto associate(fg::FrameGraph graph, Vec<PassExecutionInfo> passes,
                          GraphLimits limits = {}, slice<GraphBindingRequirement> requirements = {})
        -> Result<VulkanPassSet, GraphError> {
        auto plan = graph.check_limits({ .resources    = limits.resources,
                                         .passes       = limits.passes,
                                         .uses         = limits.uses,
                                         .dependencies = limits.dependencies });
        if (plan.is_err()) return Err(plan.unwrap_err());
        VulkanPassData data { .scope = graph.scope() };
        for (const auto& resource : graph.resources())
            data.resources.push({ .id            = resource.id,
                                  .kind          = resource.kind,
                                  .bytes         = resource.bytes,
                                  .mip_levels    = resource.mip_levels,
                                  .layers        = resource.layers,
                                  .aspects       = resource.aspects,
                                  .imported      = resource.imported,
                                  .fixed_storage = resource.storage != 0 });
        for (auto& resource : data.resources)
            resource.read_only_import =
                resource.imported && ! graph.has_resource_writes(resource.id);
        if (! requirements.is_empty()) {
            if (requirements.len() != data.resources.len())
                return Err(GraphError { GraphErrorKind::Conflict });
            for (auto& resource : data.resources) {
                bool found {};
                for (const auto& expected : requirements)
                    if (expected.id == resource.id) {
                        if (found || expected.kind != resource.kind ||
                            expected.bytes != resource.bytes ||
                            expected.mip_levels != resource.mip_levels ||
                            expected.layers != resource.layers ||
                            expected.aspects != resource.aspects ||
                            expected.imported != resource.imported ||
                            expected.fixed_storage != resource.fixed_storage)
                            return Err(GraphError { GraphErrorKind::Conflict, {}, resource.id });
                        found                     = true;
                        resource.content_revision = expected.content_revision;
                        resource.check_revision   = expected.check_revision;
                    }
                if (! found)
                    return Err(GraphError { GraphErrorKind::MissingResource, {}, resource.id });
            }
        }
        if (passes.len() != graph.passes().len())
            return Err(GraphError { GraphErrorKind::MissingPass });
        Vec<fg::Index> pass_inputs;
        pass_inputs.resize(passes.len(), fg::invalid_index);
        for (usize i {}; i < passes.len(); ++i) {
            auto id   = passes[i].id;
            auto slot = graph.pass_slot(id);
            if (slot == fg::invalid_index)
                return Err(GraphError { GraphErrorKind::MissingPass, id });
            auto& input = pass_inputs[usize(slot)];
            if (input != fg::invalid_index)
                return Err(GraphError { GraphErrorKind::Duplicate, id });
            input = fg::Index(i.to_primitive());
        }
        HashMap<u32, LogicalQueue> queues;
        for (usize slot {}; slot < graph.passes().len(); ++slot) {
            const auto& node = graph.passes()[slot];
            auto        pass = rstd::move(passes[usize(pass_inputs[slot])]);
            if (pass.queue.id != node.queue.value || pass.uses.len() != node.uses.len())
                return Err(GraphError { GraphErrorKind::Conflict, node.id });
            for (usize i {}; i < pass.uses.len(); ++i) {
                auto&       use      = pass.uses[i];
                const auto& declared = node.uses[i];
                if (use.logical_use
                        ? use.logical_use != declared.id
                        : (declared.version != fg::ResourceVersion { use.resource, use.version } ||
                           declared.access != use.access || declared.range != use.range))
                    return Err(GraphError { GraphErrorKind::Conflict, node.id, use.resource });
                use.logical_use = declared.id;
                use.resource    = declared.version.resource;
                use.version     = declared.version.version;
                use.access      = declared.access;
                use.range       = declared.range;
                use.binding     = { graph.version_index(declared.version) };
                if (use.access == GraphAccess::Write && pass.graphics) {
                    Option<GraphAttachment>* attachments[] { &pass.graphics->color,
                                                             &pass.graphics->depth,
                                                             &pass.graphics->resolve };
                    for (auto value : attachments)
                        if (*value && (**value).resource == use.resource &&
                            (**value).version == use.version) {
                            auto version = graph.find_version({ use.resource, use.version });
                            if (! version ||
                                version->available != ((**value).store == AttachmentStore::Store))
                                return Err(
                                    GraphError { GraphErrorKind::Conflict, node.id, use.resource });
                            (**value).binding = use.binding;
                        }
                }
            }
            auto valid = check_pass_execution(
                pass,
                limits,
                [&](const PassResourceUse& use) -> const GraphBindingRequirement* {
                    auto index = graph.resource_slot(use.resource);
                    return index == fg::invalid_index
                               ? nullptr
                               : rstd::addressof(data.resources[usize(index)]);
                },
                [&](fg::ResourceVersion id) {
                    return graph.allocation_slot(id);
                });
            if (valid.is_err()) return Err(valid.unwrap_err());
            auto queue = u32(pass.queue.id);
            if (auto previous = queues.get(queue)) {
                if (**previous != pass.queue)
                    return Err(GraphError { GraphErrorKind::Conflict, node.id });
            } else
                (void)queues.insert(queue, LogicalQueue(pass.queue));
            data.passes.push(rstd::move(pass));
        }
        data.dependencies = Vec<GraphDependency>::from(graph.dependencies());
        return Ok(VulkanPassSet(rstd::move(data), rstd::move(graph)));
    }
    auto same_set(const VulkanPassSet& other) const -> bool {
        return rstd::sync::Arc<Data>::ptr_eq(data_, other.data_);
    }
    auto frame_graph() const -> const fg::FrameGraph& { return data_->graph; }
    auto scope() const -> GraphSerial { return data_->graph.scope(); }
    auto binding_layout() const -> const GraphBindingLayout& { return data_->bindings; }
    auto resources() const -> slice<GraphBindingRequirement> {
        return data_->declarations.resources.as_slice();
    }
    auto passes() const -> slice<PassExecutionInfo> {
        return data_->declarations.passes.as_slice();
    }
    auto dependencies() const -> slice<GraphDependency> {
        return data_->declarations.dependencies.as_slice();
    }
    auto compile(GraphLimits limits = {}) const -> Result<GraphExecutionPlan, GraphError> {
        return data_->graph.compile({ .resources    = limits.resources,
                                      .passes       = limits.passes,
                                      .uses         = limits.uses,
                                      .dependencies = limits.dependencies });
    }
};
} // namespace vrento::vulkan
