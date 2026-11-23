export module vrento:vulkan.graph;
export import :vulkan.pass.execution;
export import :vulkan.texture;
export import :vulkan.pass.pass;
import :resource_registry.resource_key;
import rstd;

using namespace rstd::prelude;
using rstd::collections::HashMap;
using rstd::collections::HashSet;

export namespace vrento::vulkan
{
namespace graph_texture = vrento::texture;
struct GraphBindingInfo {
    GraphResourceKind       kind;
    graph_texture::Identity identity;
    GraphSerial             bytes {};
    GraphIndex              mip_levels { 1 }, layers { 1 }, aspects { 1 };
    GraphSerial             content_revision {};
};
struct GraphBinding {
    using Trait                  = GraphBinding;
    static constexpr bool direct = false;
    template<class Self, class = void>
    struct Api {
        using Trait = GraphBinding;
        auto Info() const -> GraphBindingInfo { return rstd::trait_call<0>(this); }
        auto Texture() const -> Option<graph_texture::TextureViewRef> {
            return rstd::trait_call<1>(this);
        }
        auto Use(graph_texture::Submission& work, const PassResourceUse& use) const
            -> Result<empty, GraphExecutionError> {
            return rstd::trait_call<2>(this, work, use);
        }
        auto Freeze(bool read_only) const
            -> Result<rstd::sync::Arc<dyn<GraphBinding>>, GraphExecutionError> {
            return rstd::trait_call<3>(this, read_only);
        }
        auto Release(graph_texture::Submission& work, rstd::uint32_t family,
                     const PassResourceUse& next) const -> Result<empty, GraphExecutionError> {
            return rstd::trait_call<4>(this, work, family, next);
        }
    };
    template<class T>
    using Funcs = TraitFuncs<&T::Info, &T::Texture, &T::Use, &T::Freeze, &T::Release>;
};
class GraphBindings {
    GraphBindingLayout                              layout_;
    Vec<Option<rstd::sync::Arc<dyn<GraphBinding>>>> entries_;

    struct SlotCounts {
        usize               total {};
        HashMap<u32, usize> slots;
        void                add(fg::Index slot) {
            ++total;
            if (slot == fg::invalid_index) return;
            if (auto count = slots.get_mut(u32(slot)))
                ++**count;
            else
                slots.insert(u32(slot), usize(1));
        }
        auto matches(fg::Index slot) const -> usize {
            if (slot != fg::invalid_index)
                if (auto count = slots.get(u32(slot))) return **count;
            return usize();
        }
    };
    struct AliasCounts {
        SlotCounts all, restricted;
        void       add(fg::Index slot, bool shared) {
            all.add(slot);
            if (! shared) restricted.add(slot);
        }
    };
    struct AliasGroup {
        AliasCounts                     counts;
        HashMap<fg::Index, AliasCounts> resources;
        void                            add(fg::Index resource, fg::Index slot, bool shared) {
            counts.add(slot, shared);
            if (! resources.contains_key(resource)) resources.insert(resource, AliasCounts {});
            resources.get_mut(resource).unwrap()->add(slot, shared);
        }
    };
    auto check_aliases(slice<GraphBindingRequirement> resources,
                       slice<GraphBindingInfo> infos) const -> Result<empty, GraphExecutionError> {
        const auto                                   versions = layout_.versions();
        HashMap<graph_texture::Identity, fg::Index>  owners[2];
        HashMap<graph_texture::Identity, AliasGroup> aliases[2];
        struct VersionGroup {
            HashMap<u32, graph_texture::Identity>       slots;
            HashMap<graph_texture::Identity, fg::Index> identities;
        };
        HashMap<fg::Index, VersionGroup> groups;
        auto                             check_version = [&](VersionGroup& group, usize i) {
            const auto allocation        = versions[i].allocation;
            const auto identity          = infos[i].identity;
            auto       previous_identity = group.slots.get(u32(allocation));
            auto       previous_slot     = group.identities.get(identity);
            if ((previous_identity && **previous_identity != identity) ||
                (previous_slot && **previous_slot != allocation))
                return false;
            group.slots.insert(u32(allocation), identity);
            group.identities.insert(identity, allocation);
            return true;
        };
        for (usize i {}; i < versions.len(); ++i) {
            const auto& version = versions[i];
            if (! version.used) continue;
            const auto& info = infos[i];
            const auto  kind  = info.kind == GraphResourceKind::Buffer ? 1 : 0;
            auto        owner = owners[kind].get(info.identity);
            if (! owner)
                owners[kind].insert(info.identity, version.resource);
            else if (**owner != version.resource && ! aliases[kind].contains_key(info.identity))
                aliases[kind].insert(info.identity, AliasGroup {});
            if (version.first_slotted == fg::invalid_index) continue;
            auto group = groups.get_mut(version.resource);
            if (! group) {
                groups.insert(version.resource, VersionGroup {});
                group = groups.get_mut(version.resource);
                (void)check_version(**group, usize(version.first_slotted));
            }
            if (! check_version(**group, i))
                return Err(GraphExecutionError {
                    GraphExecutionStage::Binding, version.first_pass, version.version.resource });
        }
        if (aliases[0].is_empty() && aliases[1].is_empty()) return Ok(empty {});
        for (usize i {}; i < versions.len(); ++i) {
            const auto& version = versions[i];
            if (! version.used) continue;
            const auto& info  = infos[i];
            auto&       table = aliases[info.kind == GraphResourceKind::Buffer ? 1 : 0];
            if (auto alias = table.get_mut(info.identity))
                (*alias)->add(version.resource,
                              version.allocation,
                              resources[usize(version.resource)].read_only_import);
        }
        for (usize i {}; i < versions.len(); ++i) {
            const auto& version = versions[i];
            if (! version.used) continue;
            const auto& info  = infos[i];
            const auto& table = aliases[info.kind == GraphResourceKind::Buffer ? 1 : 0];
            if (auto alias = table.get(info.identity)) {
                const auto& own    = *(*alias)->resources.get(version.resource).unwrap();
                const bool  shared = resources[usize(version.resource)].read_only_import;
                const auto& all    = shared ? (*alias)->counts.restricted : (*alias)->counts.all;
                const auto& local  = shared ? own.restricted : own.all;
                if (all.total - local.total !=
                    all.matches(version.allocation) - local.matches(version.allocation))
                    return Err(GraphExecutionError { GraphExecutionStage::Binding,
                                                     version.first_pass,
                                                     version.version.resource });
            }
        }
        return Ok(empty {});
    }

public:
    explicit GraphBindings(const GraphBindingLayout& layout): layout_(layout) {
        entries_.reserve(layout_.versions().len());
        for (usize i {}; i < layout_.versions().len(); ++i) entries_.push(None());
    }
    bool matches(const GraphBindingLayout& layout) const { return layout_.same_layout(layout); }
    auto scope() const -> GraphSerial { return layout_.scope(); }
    auto bind(GraphBindingSlot slot, rstd::sync::Arc<dyn<GraphBinding>> binding)
        -> Result<empty, GraphExecutionError> {
        if (usize(slot.index) >= entries_.len())
            return Err(GraphExecutionError { GraphExecutionStage::Binding });
        auto& entry = entries_[usize(slot.index)];
        if (entry) {
            const auto& version = layout_.versions()[usize(slot.index)];
            return Err(GraphExecutionError {
                GraphExecutionStage::Binding, version.first_pass, version.version.resource });
        }
        entry = Some(rstd::move(binding));
        return Ok(empty {});
    }
    auto resolve(GraphBindingSlot slot) const [[clang::lifetimebound]]
    -> Option<ref<dyn<GraphBinding>>> {
        if (usize(slot.index) >= entries_.len()) return None();
        const auto& entry = entries_[usize(slot.index)];
        return entry ? Some((*entry).as_ptr().as_ptr().as_ref()) : None();
    }
    auto freeze(const DeviceSession& session, slice<GraphBindingRequirement> resources,
                graph_texture::Submission* work) && -> Result<GraphBindings, GraphExecutionError> {
        auto entries = rstd::move(entries_);
        if (! session.valid() || session.lost())
            return Err(GraphExecutionError {
                GraphExecutionStage::Device,
                {},
                {},
                session.lost() ? GraphExecutionCause::Native(VK_ERROR_DEVICE_LOST)
                               : GraphExecutionCause::Reason(GraphFailure::Closed) });
        auto                  versions = layout_.versions();
        Vec<GraphBindingInfo> infos;
        infos.reserve(versions.len());
        HashSet<graph_texture::Identity> written;
        for (usize i {}; i < versions.len(); ++i) {
            const auto& version = versions[i];
            auto&       entry   = entries[i];
            if (! version.used) {
                entry = None();
                infos.push(GraphBindingInfo {});
                continue;
            }
            const auto id = version.version.resource;
            if (! entry)
                return Err(
                    GraphExecutionError { GraphExecutionStage::Binding, version.first_pass, id });
            auto info = (*entry)->Info();
            if (info.identity.device != session.epoch() || info.identity.epoch != session.epoch())
                return Err(
                    GraphExecutionError { GraphExecutionStage::Device, version.first_pass, id });
            if (! resources.is_empty()) {
                const auto& resource = resources[usize(version.resource)];
                if ((resource.check_revision && version.version.version == 0 &&
                     resource.content_revision != info.content_revision) ||
                    info.kind != resource.kind ||
                    (info.kind == GraphResourceKind::Buffer && resource.bytes > info.bytes) ||
                    (info.kind == GraphResourceKind::Texture &&
                     (resource.mip_levels > info.mip_levels || resource.layers > info.layers ||
                      (resource.aspects & ~info.aspects))))
                    return Err(GraphExecutionError {
                        GraphExecutionStage::Binding, version.first_pass, id });
            }
            if (version.written) written.insert(info.identity);
            infos.push(rstd::move(info));
        }
        if (! resources.is_empty()) {
            auto aliases = check_aliases(resources, infos.as_slice());
            if (aliases.is_err()) return Err(aliases.unwrap_err());
        }
        for (usize i {}; i < entries.len(); ++i) {
            auto& entry = entries[i];
            if (! entry) continue;
            const auto& version = versions[i];
            bool        read_only =
                (resources.is_empty() || resources[usize(version.resource)].imported) &&
                ! written.contains(infos[i].identity);
            if (read_only && work) {
                if (auto view = (*entry)->Texture()) {
                    auto recorded = texture_recorded_in(*work, session, *view);
                    if (recorded.is_err())
                        return Err(GraphExecutionError {
                            GraphExecutionStage::Binding,
                            version.first_pass,
                            version.version.resource,
                            GraphExecutionCause::Texture(recorded.unwrap_err()) });
                    read_only = ! recorded.unwrap();
                }
            }
            auto frozen = (*entry)->Freeze(read_only);
            if (frozen.is_err()) {
                auto error     = frozen.unwrap_err();
                error.resource = version.version.resource;
                if (! error.pass.value) error.pass = version.first_pass;
                return Err(rstd::move(error));
            }
            entry = Some(frozen.unwrap());
        }
        entries_ = rstd::move(entries);
        return Ok(rstd::move(*this));
    }
};
inline auto graph_stages(PassStages stages) -> graph_texture::Stages {
    graph_texture::Stages result = graph_texture::Stages::None;
    auto                  bits   = GraphIndex(stages);
    if (bits & GraphIndex(PassStages::Vertex)) result = result | graph_texture::Stages::Vertex;
    if (bits & GraphIndex(PassStages::Fragment)) result = result | graph_texture::Stages::Fragment;
    if (bits & GraphIndex(PassStages::Compute)) result = result | graph_texture::Stages::Compute;
    if (bits & GraphIndex(PassStages::Transfer)) result = result | graph_texture::Stages::Transfer;
    if (bits & GraphIndex(PassStages::Color)) result = result | graph_texture::Stages::Color;
    if (bits & GraphIndex(PassStages::Depth)) result = result | graph_texture::Stages::Depth;
    if (bits & GraphIndex(PassStages::Geometry)) result = result | graph_texture::Stages::Geometry;
    return result;
}
inline auto graph_texture_access(const PassResourceUse& use) -> Option<graph_texture::Access> {
    graph_texture::Access access;
    switch (use.usage) {
    case PassUsage::Sampled: access = graph_texture::Access::Sampled; break;
    case PassUsage::Storage:
        access = use.access == GraphAccess::Write ? graph_texture::Access::StorageWrite
                                                  : graph_texture::Access::StorageRead;
        break;
    case PassUsage::ColorAttachment: access = graph_texture::Access::ColorAttachment; break;
    case PassUsage::DepthAttachment: access = graph_texture::Access::DepthAttachment; break;
    case PassUsage::Transfer:
        access = use.access == GraphAccess::Write ? graph_texture::Access::TransferDestination
                                                  : graph_texture::Access::TransferSource;
        break;
    default: return None();
    }
    return Some(access);
}
class GraphTextureBinding {
    graph_texture::TextureViewRef                   view_;
    graph_texture::Serial                           revision_ {};
    bool                                            frozen_ {};
    Option<rstd::sync::Arc<TextureReadReservation>> reservation_;

public:
    explicit GraphTextureBinding(graph_texture::TextureViewRef view)
        : view_(rstd::move(view)),
          revision_(view_.valid() ? view_.texture().content_revision() : 0) {}
    auto Freeze(bool read_only) const
        -> Result<rstd::sync::Arc<dyn<GraphBinding>>, GraphExecutionError>;
    auto Release(graph_texture::Submission& work, rstd::uint32_t family,
                 const PassResourceUse& next) const -> Result<empty, GraphExecutionError> {
        auto native = rstd::any::downcast_ref<NativeTexture>(
                          graph_texture::BackendAccess::Storage(view_.texture())->backing.deref())
                          .unwrap();
        auto access = graph_texture_access(next);
        if (! access)
            return Err(GraphExecutionError { GraphExecutionStage::Access, {}, next.resource });
        const auto& base   = view_.description().range;
        auto        result = release_texture(
            work,
            native->session,
            { view_,
              *access,
              graph_stages(next.stages),
              false,
              Some(graph_texture::Range { next.range.aspect == 1 ? graph_texture::Aspect::Color
                                                                 : graph_texture::Aspect::Depth,
                                          base.first_mip + next.range.first_mip,
                                          next.range.mip_count,
                                          base.first_layer + next.range.first_layer,
                                          next.range.layer_count }),
              next.access == GraphAccess::Read },
            family);
        if (result.is_err())
            return Err(GraphExecutionError {
                GraphExecutionStage::Access,
                {},
                next.resource,
                vrento::vulkan::GraphExecutionCause::Texture(result.unwrap_err()) });
        return Ok(empty {});
    }
    auto Info() const -> GraphBindingInfo {
        if (! view_.valid()) return { GraphResourceKind::Texture, {} };
        const auto& range = view_.description().range;
        return { GraphResourceKind::Texture,
                 view_.texture().identity(),
                 0,
                 range.mip_count,
                 range.layer_count,
                 range.aspect == graph_texture::Aspect::Color ? 1u : 2u,
                 view_.texture().content_revision() };
    }
    auto Texture() const -> Option<graph_texture::TextureViewRef> {
        return Some(graph_texture::TextureViewRef(view_));
    }
    auto Use(graph_texture::Submission& work, const PassResourceUse& use) const
        -> Result<empty, GraphExecutionError> {
        if (frozen_ && use.access == GraphAccess::Read && use.version == 0 &&
            revision_ != view_.texture().content_revision())
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, use.resource });
        if (reservation_) {
            auto native = rstd::any::downcast_ref<NativeTexture>(
                graph_texture::BackendAccess::Storage(view_.texture())->backing.deref());
            auto attached =
                attach_prepared_texture_read(work, native.unwrap()->session, reservation_->clone());
            if (attached.is_err())
                return Err(GraphExecutionError {
                    GraphExecutionStage::Access,
                    {},
                    use.resource,
                    vrento::vulkan::GraphExecutionCause::Texture(attached.unwrap_err()) });
        }
        auto resolved = graph_texture_access(use);
        if (! resolved)
            return Err(GraphExecutionError { GraphExecutionStage::Access, {}, use.resource });
        auto        access = *resolved;
        const auto& base   = view_.description().range;
        auto        used   = work.commands()->UseTexture(
            { view_,
              access,
              graph_stages(use.stages),
              false,
              Some(graph_texture::Range { use.range.aspect == 1 ? graph_texture::Aspect::Color
                                                                : graph_texture::Aspect::Depth,
                                          base.first_mip + use.range.first_mip,
                                          use.range.mip_count,
                                          base.first_layer + use.range.first_layer,
                                          use.range.layer_count }),
              use.access == GraphAccess::Read });
        if (used.is_err())
            return Err(GraphExecutionError {
                GraphExecutionStage::Access,
                {},
                use.resource,
                vrento::vulkan::GraphExecutionCause::Texture(used.unwrap_err()) });
        return Ok(empty {});
    }
};
// BufferManager owns barriers; published writes are restricted to their submission.
class GraphUploadedBufferBinding {
    DeviceSession                      session_;
    BufferAllocation                   allocation_;
    Option<texture::SubmissionReceipt> receipt_;
    Vec<GraphRange>                    writes_;

public:
    GraphUploadedBufferBinding(DeviceSession session, BufferAllocation allocation)
        : session_(rstd::move(session)), allocation_(rstd::move(allocation)) {}
    GraphUploadedBufferBinding(DeviceSession session, BufferAllocation allocation,
                               texture::SubmissionReceipt receipt, Vec<GraphRange> writes)
        : session_(rstd::move(session)),
          allocation_(rstd::move(allocation)),
          receipt_(Some(rstd::move(receipt))),
          writes_(rstd::move(writes)) {}
    auto Info() const -> GraphBindingInfo {
        if (! session_.valid() || ! allocation_ ||
            allocation_.device() != *session_.device().handle())
            return { GraphResourceKind::Buffer, {}, 0 };
        return { GraphResourceKind::Buffer,
                 { session_.epoch(), session_.epoch(), allocation_.identity() },
                 allocation_.size() };
    }
    auto Texture() const -> Option<graph_texture::TextureViewRef> { return None(); }
    auto Release(graph_texture::Submission&, rstd::uint32_t family,
                 const PassResourceUse& next) const -> Result<empty, GraphExecutionError> {
        if (family != session_.device().graphics_queue().family_index)
            return Err(GraphExecutionError { GraphExecutionStage::Unsupported, {}, next.resource });
        return Ok(empty {});
    }
    auto Freeze(bool) const -> Result<rstd::sync::Arc<dyn<GraphBinding>>, GraphExecutionError>;
    auto Use(graph_texture::Submission& work, const PassResourceUse& use) const
        -> Result<empty, GraphExecutionError> {
        if (! session_.valid() || ! allocation_ ||
            allocation_.device() != *session_.device().handle() ||
            (! receipt_ && (use.access != GraphAccess::Read || use.version)) ||
            use.range.offset > allocation_.size() ||
            use.range.size > allocation_.size() - use.range.offset)
            return Err(GraphExecutionError { GraphExecutionStage::Unsupported, {}, use.resource });
        if (receipt_ && ! receipt_->same(work.receipt()))
            return Err(GraphExecutionError { GraphExecutionStage::Binding, {}, use.resource });
        if (use.access == GraphAccess::Write) {
            bool declared {};
            for (auto range : writes_) declared |= use.range == range;
            if (! declared || use.usage != PassUsage::Transfer ||
                use.stages != PassStages::Transfer)
                return Err(GraphExecutionError { GraphExecutionStage::Access, {}, use.resource });
        }
        auto queue = native_queue(work, session_);
        if (queue.is_err() || *queue->handle != *session_.device().graphics_queue().handle)
            return Err(GraphExecutionError { GraphExecutionStage::Unsupported, {}, use.resource });
        auto recording = native_recording(work, session_);
        if (recording.is_err())
            return Err(GraphExecutionError {
                GraphExecutionStage::Access,
                {},
                use.resource,
                vrento::vulkan::GraphExecutionCause::Texture(recording.unwrap_err()) });
        return Ok(empty {});
    }
};
struct GraphicsScopeInfo {
    VkRenderPass                 render_pass {};
    VkFramebuffer                framebuffer {};
    RenderPassResourceDesc       signature;
    VkRect2D                     area {};
    VkExtent2D                   extent {};
    rstd::array<VkClearValue, 3> clear {};
    unsigned                     clear_count {};
};
struct PreparedGraphicsScope {
    using Trait                  = PreparedGraphicsScope;
    static constexpr bool direct = false;
    template<class Self, class = void>
    struct Api {
        using Trait = PreparedGraphicsScope;
        auto Info() const -> GraphicsScopeInfo { return rstd::trait_call<0>(this); }
    };
    template<class T>
    using Funcs = TraitFuncs<&T::Info>;
};
struct GraphPrepareContext {
    const DeviceSession&                                session;
    const GraphBindings&                                bindings;
    Option<rstd::sync::Arc<dyn<PreparedGraphicsScope>>> scope;
};
struct GraphRecordContext {
    const DeviceSession&                                session;
    const GraphBindings&                                bindings;
    const PassExecutionInfo&                            pass;
    graph_texture::Submission&                          submission;
    Option<rstd::sync::Arc<dyn<PreparedGraphicsScope>>> scope;
    auto texture(GraphBindingSlot slot) const -> Option<graph_texture::TextureViewRef> {
        auto bound = bindings.resolve(slot);
        return bound.is_some() ? (*bound)->Texture() : None<graph_texture::TextureViewRef>();
    }
};
struct GraphicsScopeBackend {
    using Trait                  = GraphicsScopeBackend;
    static constexpr bool direct = false;
    template<class Self, class = void>
    struct Api {
        using Trait = GraphicsScopeBackend;
        auto Prepare(const GraphicsDescription& description, const GraphPrepareContext& context)
            -> Result<vrento::vulkan::Preparation<rstd::sync::Arc<dyn<PreparedGraphicsScope>>>,
                      GraphExecutionError> {
            return rstd::trait_call<0>(this, description, context);
        }
    };
    template<class T>
    using Funcs = TraitFuncs<&T::Prepare>;
};
struct VulkanPassBackend {
    using Trait                  = VulkanPassBackend;
    static constexpr bool direct = false;
    template<class Self, class = void>
    struct Api {
        using Trait = VulkanPassBackend;
        auto Prepare(const PassExecutionInfo& pass, const GraphPrepareContext& context)
            -> Result<vrento::vulkan::Preparation<rstd::sync::Arc<dyn<VulkanPass>>>,
                      GraphExecutionError> {
            return rstd::trait_call<0>(this, pass, context);
        }
    };
    template<class T>
    using Funcs = TraitFuncs<&T::Prepare>;
};
class VulkanPassBackends {
    struct Entry {
        GraphSerial                 kind;
        GraphIndex                  schema;
        Box<dyn<VulkanPassBackend>> backend;
    };
    Vec<Entry>                             entries_;
    Option<Box<dyn<GraphicsScopeBackend>>> scopes_;

public:
    void set_scopes(Box<dyn<GraphicsScopeBackend>> scopes) { scopes_ = Some(rstd::move(scopes)); }
    auto prepare_scope(const GraphicsDescription& description, const GraphPrepareContext& context)
        -> Result<vrento::vulkan::Preparation<rstd::sync::Arc<dyn<PreparedGraphicsScope>>>,
                  GraphExecutionError> {
        if (scopes_.is_none()) return Err(GraphExecutionError { GraphExecutionStage::Unsupported });
        return (*scopes_)->Prepare(description, context);
    }
    auto bind(GraphSerial kind, GraphIndex schema, Box<dyn<VulkanPassBackend>> backend)
        -> Result<empty, GraphExecutionError> {
        if (! kind || ! schema) return Err(GraphExecutionError { GraphExecutionStage::Schema });
        for (const auto& entry : entries_)
            if (entry.kind == kind && entry.schema == schema)
                return Err(GraphExecutionError { GraphExecutionStage::Schema });
        entries_.push({ kind, schema, rstd::move(backend) });
        return Ok(empty {});
    }
    auto resolve_passes(const VulkanPassSet& graph, slice<GraphIndex> order)
        -> Result<rstd::sync::Arc<Vec<usize>>, GraphExecutionError> {
        Vec<usize> routes;
        routes.resize(graph.passes().len(), usize::MAX);
        for (auto index : order) {
            if (usize(index) >= graph.passes().len())
                return Err(GraphExecutionError { GraphExecutionStage::Schema });
            const auto& pass = graph.passes()[usize(index)];
            for (usize i {}; i < entries_.len(); ++i)
                if (entries_[i].kind == pass.kind && entries_[i].schema == pass.schema_version) {
                    routes[usize(index)] = i;
                    break;
                }
            if (routes[usize(index)] == usize::MAX)
                return Err(GraphExecutionError { GraphExecutionStage::Schema, pass.id });
        }
        return Ok(rstd::sync::Arc<Vec<usize>>::make(rstd::move(routes)));
    }
    auto prepare_at(usize slot, const PassExecutionInfo& pass, const GraphPrepareContext& context)
        -> Result<vrento::vulkan::Preparation<rstd::sync::Arc<dyn<VulkanPass>>>,
                  GraphExecutionError> {
        if (slot >= entries_.len() || entries_[slot].kind != pass.kind ||
            entries_[slot].schema != pass.schema_version)
            return Err(GraphExecutionError { GraphExecutionStage::Schema, pass.id });
        return entries_[slot].backend->Prepare(pass, context);
    }
    auto prepare(const PassExecutionInfo& pass, const GraphPrepareContext& context)
        -> Result<vrento::vulkan::Preparation<rstd::sync::Arc<dyn<VulkanPass>>>,
                  GraphExecutionError> {
        for (usize i {}; i < entries_.len(); ++i)
            if (entries_[i].kind == pass.kind && entries_[i].schema == pass.schema_version)
                return prepare_at(i, pass, context);
        return Err(GraphExecutionError { GraphExecutionStage::Schema, pass.id });
    }
};
inline auto record_pass_access(const PassExecutionInfo& pass, const GraphBindings& bindings,
                               graph_texture::Submission& work)
    -> Result<empty, GraphExecutionError> {
    for (unsigned writing {}; writing < 2; ++writing)
        for (const auto& use : pass.uses) {
            if ((use.access == GraphAccess::Write) != bool(writing)) continue;
            auto binding = bindings.resolve(use.binding);
            if (binding.is_none())
                return Err(
                    GraphExecutionError { GraphExecutionStage::Binding, pass.id, use.resource });
            auto used = (*binding)->Use(work, use);
            if (used.is_err()) return Err(used.unwrap_err());
        }
    return Ok(empty {});
}
inline auto record_prepared_pass(const DeviceSession& session, const PassExecutionInfo& pass,
                                 const GraphBindings&                    bindings,
                                 const rstd::sync::Arc<dyn<VulkanPass>>& prepared,
                                 const Option<rstd::sync::Arc<dyn<PreparedGraphicsScope>>>& scope,
                                 graph_texture::Submission& work, bool enter = true,
                                 bool leave = true, bool access = true)
    -> Result<empty, GraphExecutionError> {
    auto fail = [&](GraphExecutionError error) -> Result<empty, GraphExecutionError> {
        error.pass = pass.id;
        work.discard();
        return Err(rstd::move(error));
    };
    auto recording = native_recording(work, session);
    if (recording.is_err())
        return fail(GraphExecutionError {
            GraphExecutionStage::Record,
            {},
            {},
            vrento::vulkan::GraphExecutionCause::Texture(recording.unwrap_err()) });
    auto queue = native_queue(work, session).unwrap();
    if ((pass.queue.capability == QueueClass::Graphics &&
         ! (queue.flags & VK_QUEUE_GRAPHICS_BIT)) ||
        (pass.queue.capability == QueueClass::Compute && ! (queue.flags & VK_QUEUE_COMPUTE_BIT)) ||
        (pass.queue.capability == QueueClass::Transfer &&
         ! (queue.flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))))
        return fail(GraphExecutionError { GraphExecutionStage::Unsupported });
    if (access) {
        auto accessed = record_pass_access(pass, bindings, work);
        if (accessed.is_err()) return fail(accessed.unwrap_err());
    }
    GraphRecordContext context { session, bindings, pass, work, scope.clone() };
    const auto&        dispatch = session.device().handle().Dispatch();
    if (scope && enter) {
        auto                  info = (*scope)->Info();
        VkRenderPassBeginInfo begin { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        begin.renderPass      = info.render_pass;
        begin.framebuffer     = info.framebuffer;
        begin.renderArea      = info.area;
        begin.clearValueCount = info.clear_count;
        begin.pClearValues    = info.clear.data();
        dispatch.vkCmdBeginRenderPass(recording.unwrap(), &begin, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport { float(info.area.offset.x),
                              float(info.area.offset.y),
                              float(info.area.extent.width),
                              float(info.area.extent.height),
                              0,
                              1 };
        dispatch.vkCmdSetViewport(recording.unwrap(), 0, 1, &viewport);
        dispatch.vkCmdSetScissor(recording.unwrap(), 0, 1, &info.area);
    }
    if (scope && ! enter) {
        auto       info = (*scope)->Info();
        VkViewport viewport { float(info.area.offset.x),
                              float(info.area.offset.y),
                              float(info.area.extent.width),
                              float(info.area.extent.height),
                              0,
                              1 };
        dispatch.vkCmdSetViewport(recording.unwrap(), 0, 1, &viewport);
        dispatch.vkCmdSetScissor(recording.unwrap(), 0, 1, &info.area);
    }
    auto recorded = prepared->Record(context);
    if (scope && (leave || recorded.is_err())) dispatch.vkCmdEndRenderPass(recording.unwrap());
    if (recorded.is_err()) return fail(recorded.unwrap_err());
    if (pass.graphics && leave) {
        const Option<GraphAttachment>* attachments[] { &pass.graphics->color,
                                                       &pass.graphics->depth,
                                                       &pass.graphics->resolve };
        for (const auto* attachment : attachments) {
            if (attachment->is_none() || (**attachment).store != AttachmentStore::Discard) continue;
            auto binding = bindings.resolve((**attachment).binding);
            if (! binding) return fail(GraphExecutionError { GraphExecutionStage::Binding });
            auto view    = (*binding)->Texture();
            if (view.is_none()) return fail(GraphExecutionError { GraphExecutionStage::Binding });
            auto discarded = discard_texture_contents(work, session, *view);
            if (discarded.is_err())
                return fail(GraphExecutionError {
                    GraphExecutionStage::Access,
                    {},
                    (**attachment).resource,
                    vrento::vulkan::GraphExecutionCause::Texture(discarded.unwrap_err()) });
        }
    }
    return Ok(empty {});
}
template<typename Backends>
inline auto record_vulkan_pass(DeviceSession session, const PassExecutionInfo& pass,
                               GraphBindings bindings, Backends& backends,
                               graph_texture::Submission& work)
    -> Result<FrameProgress, GraphExecutionError> {
    auto fail = [&](GraphExecutionError error) -> Result<FrameProgress, GraphExecutionError> {
        error.pass = pass.id;
        work.discard();
        return Err(rstd::move(error));
    };
    GraphBindingRequirement                      requirement;
    HashMap<graph_texture::Identity, ResourceId> owners[2];
    for (const auto& use : pass.uses) {
        auto binding = bindings.resolve(use.binding);
        if (! binding)
            return fail(
                GraphExecutionError { GraphExecutionStage::Binding, pass.id, use.resource });
        auto  info  = (*binding)->Info();
        auto& table = owners[info.kind == GraphResourceKind::Buffer ? 1 : 0];
        if (auto owner = table.get(info.identity)) {
            if (**owner != use.resource)
                return fail(
                    GraphExecutionError { GraphExecutionStage::Binding, pass.id, use.resource });
        } else {
            table.insert(info.identity, use.resource);
        }
    }
    auto valid = check_pass_execution(
        pass,
        {},
        [&](const PassResourceUse& use) {
            auto info   = bindings.resolve(use.binding).unwrap()->Info();
            requirement = { use.resource, info.kind,    info.bytes, info.mip_levels,
                            info.layers,  info.aspects, true };
            return &requirement;
        },
        [](fg::ResourceVersion) {
            return fg::invalid_index;
        });
    if (valid.is_err())
        return fail(
            GraphExecutionError { GraphExecutionStage::Graph,
                                  {},
                                  {},
                                  vrento::vulkan::GraphExecutionCause::Graph(valid.unwrap_err()) });
    auto frozen = rstd::move(bindings).freeze(session, {}, &work);
    if (frozen.is_err()) return fail(frozen.unwrap_err());
    auto flags = session.device().graphics_queue().flags;
    if (pass.queue.capability == QueueClass::Compute && ! (flags & VK_QUEUE_COMPUTE_BIT))
        return fail(GraphExecutionError { GraphExecutionStage::Unsupported });
    GraphPrepareContext context { session, *frozen };
    if (pass.graphics) {
        auto scope = backends.prepare_scope(*pass.graphics, context);
        if (scope.is_err()) return fail(scope.unwrap_err());
        if (! scope->ready()) return Ok(FrameProgress::Pending);
        context.scope = Some(scope->take_ready());
    }
    auto prepared = backends.prepare(pass, context);
    if (prepared.is_err()) return fail(prepared.unwrap_err());
    struct Lease {
        DeviceSession                                       session;
        GraphBindings                                       bindings;
        rstd::sync::Arc<dyn<VulkanPass>>                    pass;
        Option<rstd::sync::Arc<dyn<PreparedGraphicsScope>>> scope;
    };
    if (! prepared->ready()) return Ok(FrameProgress::Pending);
    auto instance = prepared->take_ready();
    auto recorded = record_prepared_pass(session, pass, *frozen, instance, context.scope, work);
    if (recorded.is_err()) return Err(recorded.unwrap_err());
    work.retain(Lease { session, frozen.unwrap(), instance.clone(), context.scope.clone() });
    return Ok(FrameProgress::Ready);
}
inline auto merge_graphics(const PassExecutionInfo& previous, const PassExecutionInfo& next)
    -> bool {
    if (! previous.graphics || ! next.graphics || previous.queue != next.queue) return false;
    const auto& a = *previous.graphics;
    const auto& b = *next.graphics;
    if (a.x != b.x || a.y != b.y || a.width != b.width || a.height != b.height ||
        a.full_target != b.full_target)
        return false;
    const Option<GraphAttachment>* left[] { &a.color, &a.depth, &a.resolve };
    const Option<GraphAttachment>* right[] { &b.color, &b.depth, &b.resolve };
    for (unsigned role {}; role < 3; ++role) {
        if (left[role]->is_some() != right[role]->is_some()) return false;
        if (! *left[role]) continue;
        const auto& before = **left[role];
        const auto& after  = **right[role];
        if (before.resource != after.resource ||
            (role != 2 &&
             (before.store != AttachmentStore::Store || after.load != AttachmentLoad::Load)) ||
            (role == 2 && after.load != AttachmentLoad::Discard))
            return false;
    }
    const PassExecutionInfo* passes[] { &previous, &next };
    for (const auto* pass : passes)
        for (const auto& use : pass->uses) {
            bool attachment {};
            for (const auto* declared : left)
                attachment |= declared->is_some() && use.resource == (**declared).resource;
            if (attachment && use.usage != PassUsage::ColorAttachment &&
                use.usage != PassUsage::DepthAttachment)
                return false;
            if (! attachment && use.access == GraphAccess::Write) return false;
        }
    return true;
}
class PreparedGraphProgram {
    struct Data {
        DeviceSession                                            session;
        VulkanPassSet                                            graph;
        const GraphExecutionPlan&                                plan;
        Option<rstd::sync::Arc<Vec<usize>>>                      routes;
        GraphBindings                                            bindings;
        Vec<rstd::sync::Arc<dyn<VulkanPass>>>                    passes;
        Vec<Option<rstd::sync::Arc<dyn<PreparedGraphicsScope>>>> scopes;
        Vec<usize>                                               ends;
        usize                                                    scope_count {};
        bool                                                     merge_scopes {}, group_ready {};
        usize                                                    group_end {};
        Option<rstd::sync::Arc<dyn<PreparedGraphicsScope>>>      group_scope;
        Option<graph_texture::SubmissionReceipt>                 receipt;
        Data(DeviceSession device, VulkanPassSet description, GraphBindings resources)
            : session(rstd::move(device)),
              graph(rstd::move(description)),
              plan(graph.frame_graph().inspect()),
              bindings(rstd::move(resources)) {}
    };
    rstd::sync::Arc<Data> data_;
    explicit PreparedGraphProgram(rstd::sync::Arc<Data> data): data_(rstd::move(data)) {}

public:
    PreparedGraphProgram(const PreparedGraphProgram& other): data_(other.data_.clone()) {}
    PreparedGraphProgram(PreparedGraphProgram&&)                           = default;
    auto        operator=(PreparedGraphProgram&&) -> PreparedGraphProgram& = default;
    static auto begin_prepare(DeviceSession session, VulkanPassSet graph, GraphBindings bindings,
                              bool merge_scopes = true, graph_texture::Submission* work = nullptr)
        -> Result<PreparedGraphProgram, GraphExecutionError> {
        if (! bindings.matches(graph.binding_layout()))
            return Err(GraphExecutionError { GraphExecutionStage::Binding });
        auto frozen = rstd::move(bindings).freeze(session, graph.resources(), work);
        if (frozen.is_err()) return Err(frozen.unwrap_err());
        auto data = rstd::sync::Arc<Data>::make(rstd::move(session),
                                                rstd::move(graph),
                                                rstd::move(frozen).unwrap());
        if (work) data->receipt = Some(work->receipt());
        data->merge_scopes = merge_scopes;
        return Ok(PreparedGraphProgram(rstd::move(data)));
    }
    template<typename Backends>
    auto continue_prepare(Backends& backends) -> Result<FrameProgress, GraphExecutionError> {
        auto& data = *data_;
        if (data.session.lost())
            return Err(GraphExecutionError { GraphExecutionStage::Device,
                                             {},
                                             {},
                                             GraphExecutionCause::Native(VK_ERROR_DEVICE_LOST) });
        if (data.receipt &&
            (data.receipt->cancelled() || data.receipt->submitted() || data.receipt->uncertain()))
            return Err(GraphExecutionError { GraphExecutionStage::Prepare });
        if (! data.routes) {
            auto routes = backends.resolve_passes(data.graph, data.plan.order.as_slice());
            if (routes.is_err()) return Err(routes.unwrap_err());
            if ((**routes).len() != data.graph.passes().len())
                return Err(GraphExecutionError { GraphExecutionStage::Schema });
            data.routes = Some(routes.unwrap());
        }
        GraphPrepareContext context { data.session, data.bindings };
        while (data.passes.len() < data.plan.order.len()) {
            auto        position = data.passes.len();
            const auto& pass     = data.graph.passes()[usize(data.plan.order[position])];
            if (! data.group_ready) {
                usize last = position;
                while (data.merge_scopes && last + usize(1) < data.plan.order.len() &&
                       merge_graphics(data.graph.passes()[usize(data.plan.order[last])],
                                      data.graph.passes()[usize(data.plan.order[last + usize(1)])]))
                    ++last;
                data.group_scope = None();
                context.scope    = None();
                if (pass.graphics) {
                    auto        description = GraphicsDescription(*pass.graphics);
                    const auto& final = *data.graph.passes()[usize(data.plan.order[last])].graphics;
                    if (description.color) description.color->store = final.color->store;
                    if (description.depth) description.depth->store = final.depth->store;
                    if (description.resolve) description.resolve->store = final.resolve->store;
                    auto scope = backends.prepare_scope(description, context);
                    if (scope.is_err()) {
                        auto error = scope.unwrap_err();
                        error.pass = pass.id;
                        return Err(rstd::move(error));
                    }
                    if (! scope->ready()) return Ok(FrameProgress::Pending);
                    data.group_scope = Some(scope->take_ready());
                    ++data.scope_count;
                }
                data.group_end   = last;
                data.group_ready = true;
            }
            context.scope = data.group_scope.clone();
            auto prepared = backends.prepare_at(
                (**data.routes)[usize(data.plan.order[position])], pass, context);
            if (prepared.is_err()) {
                auto error = prepared.unwrap_err();
                error.pass = pass.id;
                return Err(rstd::move(error));
            }
            if (! prepared->ready()) return Ok(FrameProgress::Pending);
            data.passes.push(prepared->take_ready());
            data.scopes.push(data.group_scope.clone());
            data.ends.push(usize(data.group_end));
            if (position == data.group_end) {
                data.group_ready = false;
                data.group_scope = None();
            }
        }
        return Ok(FrameProgress::Ready);
    }
    static auto prepare(DeviceSession session, VulkanPassSet graph, GraphBindings bindings,
                        VulkanPassBackends& backends, bool merge_scopes = true,
                        graph_texture::Submission* work = nullptr)
        -> Result<PreparedGraphProgram, GraphExecutionError> {
        auto started = begin_prepare(
            rstd::move(session), rstd::move(graph), rstd::move(bindings), merge_scopes, work);
        if (started.is_err()) return Err(started.unwrap_err());
        auto program  = started.unwrap();
        auto advanced = program.continue_prepare(backends);
        if (advanced.is_err()) return Err(advanced.unwrap_err());
        return Ok(rstd::move(program));
    }
    auto ready() const -> bool { return data_->passes.len() == data_->plan.order.len(); }
    auto scope_count() const -> usize { return data_->scope_count; }
    auto description() const -> const VulkanPassSet& { return data_->graph; }
    auto schedule() const -> const GraphExecutionPlan& { return data_->plan; }
    auto binding(GraphBindingSlot slot) const -> Option<ref<dyn<GraphBinding>>> {
        return data_->bindings.resolve(slot);
    }
    auto record_batch(usize batch, graph_texture::Submission& work) const
        -> Result<empty, GraphExecutionError> {
        if (! ready()) return Err(GraphExecutionError { GraphExecutionStage::Record });
        if (data_->receipt && ! data_->receipt->same(work.receipt()))
            return Err(GraphExecutionError { GraphExecutionStage::Binding });
        if (batch >= data_->plan.batches.len() ||
            data_->plan.batches[batch].passes.len() != usize(1) || data_->ends[batch] != batch ||
            (batch > usize() && data_->ends[batch - usize(1)] == batch))
            return Err(GraphExecutionError { GraphExecutionStage::Unsupported });
        work.retain(data_.clone());
        const auto& pass = data_->graph.passes()[usize(data_->plan.batches[batch].passes[usize()])];
        return record_prepared_pass(data_->session,
                                    pass,
                                    data_->bindings,
                                    data_->passes[batch],
                                    data_->scopes[batch],
                                    work);
    }
    auto record(graph_texture::Submission& work) const -> Result<empty, GraphExecutionError> {
        if (! ready()) return Err(GraphExecutionError { GraphExecutionStage::Record });
        if (data_->receipt && ! data_->receipt->same(work.receipt()))
            return Err(GraphExecutionError { GraphExecutionStage::Binding });
        if (data_->session.lost())
            return Err(GraphExecutionError {
                GraphExecutionStage::Device,
                {},
                {},
                vrento::vulkan::GraphExecutionCause::Native(VK_ERROR_DEVICE_LOST) });
        auto recording = native_recording(work, data_->session);
        if (recording.is_err())
            return Err(GraphExecutionError {
                GraphExecutionStage::Record,
                {},
                {},
                vrento::vulkan::GraphExecutionCause::Texture(recording.unwrap_err()) });
        // Hold the whole execution snapshot even if a later recorder fails.
        work.retain(data_.clone());
        for (usize first {}; first < data_->plan.order.len();) {
            auto last = usize(data_->ends[first]);
            if (last > first)
                for (usize index = first; index <= last; ++index) {
                    const auto& pass     = data_->graph.passes()[usize(data_->plan.order[index])];
                    auto        accessed = record_pass_access(pass, data_->bindings, work);
                    if (accessed.is_err()) {
                        auto error = accessed.unwrap_err();
                        error.pass = pass.id;
                        work.discard();
                        return Err(rstd::move(error));
                    }
                }
            for (usize index = first; index <= last; ++index) {
                const auto& pass     = data_->graph.passes()[usize(data_->plan.order[index])];
                auto        recorded = record_prepared_pass(data_->session,
                                                            pass,
                                                            data_->bindings,
                                                            data_->passes[index],
                                                            data_->scopes[index],
                                                            work,
                                                            index == first,
                                                            index == last,
                                                            first == last);
                if (recorded.is_err()) return Err(recorded.unwrap_err());
            }
            first = last + usize(1);
        }
        return Ok(empty {});
    }
};
} // namespace vrento::vulkan

export namespace rstd
{
template<>
struct Impl<vrento::vulkan::GraphBinding, vrento::vulkan::GraphUploadedBufferBinding>
    : ImplBase<vrento::vulkan::GraphUploadedBufferBinding> {
    auto Info() const -> vrento::vulkan::GraphBindingInfo { return self().Info(); }
    auto Texture() const -> Option<vrento::texture::TextureViewRef> { return self().Texture(); }
    auto Use(vrento::texture::Submission& work, const vrento::vulkan::PassResourceUse& use) const
        -> Result<empty, vrento::vulkan::GraphExecutionError> {
        return self().Use(work, use);
    }
    auto Release(vrento::texture::Submission& work, rstd::uint32_t family,
                 const vrento::vulkan::PassResourceUse& next) const
        -> Result<empty, vrento::vulkan::GraphExecutionError> {
        return self().Release(work, family, next);
    }
    auto Freeze(bool read_only) const -> Result<sync::Arc<dyn<vrento::vulkan::GraphBinding>>,
                                                vrento::vulkan::GraphExecutionError> {
        return self().Freeze(read_only);
    }
};
template<>
struct Impl<vrento::vulkan::GraphBinding, vrento::vulkan::GraphTextureBinding>
    : ImplBase<vrento::vulkan::GraphTextureBinding> {
    auto Info() const -> vrento::vulkan::GraphBindingInfo { return self().Info(); }
    auto Texture() const -> Option<vrento::texture::TextureViewRef> { return self().Texture(); }
    auto Use(vrento::texture::Submission& work, const vrento::vulkan::PassResourceUse& use) const
        -> Result<empty, vrento::vulkan::GraphExecutionError> {
        return self().Use(work, use);
    }
    auto Release(vrento::texture::Submission& work, rstd::uint32_t family,
                 const vrento::vulkan::PassResourceUse& next) const
        -> Result<empty, vrento::vulkan::GraphExecutionError> {
        return self().Release(work, family, next);
    }
    auto Freeze(bool read_only) const -> Result<sync::Arc<dyn<vrento::vulkan::GraphBinding>>,
                                                vrento::vulkan::GraphExecutionError> {
        return self().Freeze(read_only);
    }
};
} // namespace rstd
namespace vrento::vulkan
{
auto GraphUploadedBufferBinding::Freeze(bool) const
    -> Result<rstd::sync::Arc<dyn<GraphBinding>>, GraphExecutionError> {
    return receipt_ ? Ok(rstd::sync::Arc<dyn<GraphBinding>>::make(GraphUploadedBufferBinding(
                          session_, allocation_.clone(), *receipt_, writes_.clone())))
                    : Ok(rstd::sync::Arc<dyn<GraphBinding>>::make(
                          GraphUploadedBufferBinding(session_, allocation_.clone())));
}
auto GraphTextureBinding::Freeze(bool read_only) const
    -> Result<rstd::sync::Arc<dyn<GraphBinding>>, GraphExecutionError> {
    GraphTextureBinding snapshot(view_);
    snapshot.frozen_ = true;
    if (read_only) {
        auto native = rstd::any::downcast_ref<NativeTexture>(
            graph_texture::BackendAccess::Storage(view_.texture())->backing.deref());
        if (native.is_none()) return Err(GraphExecutionError { GraphExecutionStage::Device });
        auto reserved = reserve_texture_read(native.unwrap()->session, view_);
        if (reserved.is_err())
            return Err(GraphExecutionError {
                GraphExecutionStage::Binding,
                {},
                {},
                vrento::vulkan::GraphExecutionCause::Texture(reserved.unwrap_err()) });
        snapshot.reservation_ = Some(rstd::move(reserved).unwrap());
    }
    return Ok(rstd::sync::Arc<dyn<GraphBinding>>::make(rstd::move(snapshot)));
}
} // namespace vrento::vulkan
