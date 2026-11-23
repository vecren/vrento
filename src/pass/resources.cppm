export module vrento:pass.resources;
import rstd;
import :frame_graph;
export import :pass.content;
import :resource;
import :texture.core;

using namespace rstd::prelude;

export namespace vrento::pass
{
namespace fg       = vrento::fg;
namespace resource = vrento::resource;

struct TextureSpec {
    String                       key;
    bool                         imported {};
    resource::TextureResourceRef definition;
    auto request() const -> const resource::TextureRequest& { return definition.definition(); }
    Option<String>                        allocation_family;
    Option<vrento::texture::TextureDesc>  metadata;
    Option<resource::TextureDefinitionId> source;
    bool                                  initialize_history_transparent {};
    auto                                  source_request() const -> resource::TextureRequest {
        auto result = request().clone();
        if (source) result.source = source;
        return result;
    }
    auto clone() const -> TextureSpec {
        return { key.clone(),
                 imported,
                 definition.clone(),
                 allocation_family.clone(),
                 metadata,
                 source,
                 initialize_history_transparent };
    }
};
struct TextureUse {
    fg::ResourceVersion        version;
    resource::TextureUseHandle handle;
};
struct GraphTexture {
    fg::ResourceId id;
    TextureSpec    spec;
};
struct BufferUse {
    resource::BufferUseHandle use;
    fg::ResourceVersion       version;
};
struct TextureBinding {
    TextureUse        resource;
    fg::ResourceRange range;
    resource::TextureResourceRef definition;
};
struct BufferBinding {
    BufferUse         resource;
    fg::ResourceRange range;
    resource::BufferResourceRef definition;
};
struct GraphResourceMap {
    fg::Serial           scope {};
    Vec<TextureBinding>  textures;
    Vec<BufferBinding>   buffers;
    Vec<fg::ResourceVersion> externals;
    auto                 same_bindings(const GraphResourceMap& other) const -> bool {
        if (scope != other.scope || textures.len() != other.textures.len() ||
            buffers.len() != other.buffers.len() || externals.len() != other.externals.len())
            return false;
        for (usize i {}; i < textures.len(); ++i) {
            const auto& a = textures[i];
            const auto& b = other.textures[i];
            if (a.resource.version != b.resource.version ||
                a.resource.handle != b.resource.handle || a.range != b.range ||
                ! a.definition.valid() || ! b.definition.valid() ||
                a.definition.id() != b.definition.id() ||
                a.definition.definition().definition != b.definition.definition().definition ||
                a.definition.definition().kind != b.definition.definition().kind)
                return false;
        }
        for (usize i {}; i < buffers.len(); ++i) {
            const auto& a = buffers[i];
            const auto& b = other.buffers[i];
            if (a.resource.version != b.resource.version || a.resource.use != b.resource.use ||
                a.range != b.range || ! a.definition.valid() || ! b.definition.valid() ||
                a.definition.id() != b.definition.id() ||
                a.definition.definition().definition != b.definition.definition().definition)
                return false;
        }
        for (usize i {}; i < externals.len(); ++i) {
            if (externals[i] != other.externals[i]) return false;
        }
        return true;
    }
};
class GraphResources {
    u64               generation_;
    Vec<GraphTexture> textures_;
    Vec<TextureUse>   uses_;
    Vec<BufferUse>    buffers_;
    HashMap<fg::ResourceId, usize>            textures_by_id_, buffers_by_id_;
    HashMap<fg::ResourceVersion, usize>       uses_by_version_;
    HashMap<resource::BufferUseHandle, usize> buffers_by_use_;

public:
    explicit GraphResources(u64 generation): generation_(generation) {}
    auto content(const fg::FrameGraphBuilder& graph, fg::ResourceVersion version) const
        -> Result<ContentRef, fg::Error> {
        if (auto texture = find(version.resource))
            return ContentRef::bind(graph, version, Some(texture->spec.definition.clone()));
        return ContentRef::bind(graph, version);
    }
    auto initial(fg::FrameGraphBuilder& graph, TextureSpec spec) -> Result<ContentRef, fg::Error> {
        auto version = declare(graph, rstd::move(spec));
        if (version.is_err()) return Err(version.unwrap_err());
        return content(graph, *version);
    }
    auto output(fg::FrameGraphBuilder& graph, fg::PassId pass, const ContentRef& previous,
                bool reuse = false, bool inherits = false) -> Result<ContentRef, fg::Error> {
        return ContentRef::write(graph, pass, previous, reuse, inherits);
    }
    auto handle(const ContentRef& content) -> Result<resource::TextureUseHandle, fg::Error> {
        auto version = content.resolve(generation_.to_primitive());
        return version.is_ok() ? handle(*version) : Err(version.unwrap_err());
    }
    auto content(const fg::FrameGraphBuilder& graph, resource::TextureUseHandle use) const
        -> Result<ContentRef, fg::Error> {
        auto version = this->version(use);
        if (! version) return Err(fg::Error { fg::ErrorKind::MissingResource });
        return content(graph, *version);
    }
    auto find(const ContentRef& content) const -> const GraphTexture* {
        auto version = content.resolve(generation_.to_primitive());
        return version.is_ok() ? find(version->resource) : nullptr;
    }
    auto shape(const fg::FrameGraphBuilder& graph, const ContentRef& content) const
        -> const fg::ResourceDescription* {
        auto version = content.resolve(graph.data().scope);
        return version.is_ok() ? graph.find_resource(version->resource) : nullptr;
    }
    auto import_history(fg::FrameGraphBuilder& graph, const ContentRef& content)
        -> Result<empty, fg::Error> {
        auto version = content.resolve(graph.data().scope);
        if (version.is_err()) return Err(version.unwrap_err());
        if (! content.initial()) return Ok(empty {});
        return graph.import_initial(version->resource);
    }
    auto reuse_storage(fg::FrameGraphBuilder& graph, const ContentRef& content)
        -> Result<empty, fg::Error> {
        auto version = content.resolve(graph.data().scope);
        if (version.is_err()) return Err(version.unwrap_err());
        return graph.reuse_storage(*version);
    }
    auto buffers() const -> slice<BufferUse> { return buffers_.as_slice(); }
    auto declare_buffers(fg::FrameGraphBuilder& graph, slice<resource::BufferPlanEntry> entries)
        -> Result<empty, fg::Error> {
        if (graph.data().scope != generation_.to_primitive())
            return Err(fg::Error { fg::ErrorKind::Conflict });
        for (const auto& entry : entries) {
            if (! entry.definition.valid())
                return Err(fg::Error { fg::ErrorKind::MissingResource });
            if (buffers_by_use_.get(entry.handle) || entry.handle.generation != generation_)
                return Err(fg::Error { fg::ErrorKind::Conflict });
            auto           id = graph.next_resource_id();
            auto           declared =
                graph.import_resource({ .id      = id,
                                        .kind    = fg::ResourceKind::Buffer,
                                        .bytes   = entry.request().definition.size.to_primitive(),
                                        .spec    = id.value,
                                        .storage = id.value,
                                        .name    = entry.request().name.clone() });
            if (declared.is_err()) return Err(declared.unwrap_err());
            (void)buffers_by_id_.insert(id, buffers_.len());
            (void)buffers_by_use_.insert(entry.handle, buffers_.len());
            buffers_.push({ entry.handle, declared.unwrap() });
        }
        return Ok(empty {});
    }
    auto version(resource::BufferUseHandle handle) const -> Option<fg::ResourceVersion> {
        auto index = buffers_by_use_.get(handle);
        return index ? Some(fg::ResourceVersion(buffers_[**index].version)) : None();
    }
    auto generation() const -> u64 { return generation_; }
    auto textures() const -> slice<GraphTexture> { return textures_.as_slice(); }
    auto uses() const -> slice<TextureUse> { return uses_.as_slice(); }
    auto find(fg::ResourceId id) const -> const GraphTexture* {
        auto index = textures_by_id_.get(id);
        return index ? rstd::addressof(textures_[**index]) : nullptr;
    }
    auto find(ref<str> name) const -> const GraphTexture* {
        for (const auto& entry : textures_)
            if (entry.spec.key == name) return rstd::addressof(entry);
        return nullptr;
    }
    auto logical(fg::ResourceId id) const -> Option<resource::TextureResourceRef> {
        auto resource = find(id);
        return resource ? Some(resource->spec.definition.clone())
                        : None<resource::TextureResourceRef>();
    }
    auto handle(fg::ResourceVersion version) -> Result<resource::TextureUseHandle, fg::Error> {
        if (! find(version.resource))
            return Err(fg::Error { fg::ErrorKind::MissingResource, {}, version.resource });
        if (auto index = uses_by_version_.get(version)) return Ok(uses_[**index].handle);
        resource::TextureUseHandle result { rstd::as_cast<u64>(uses_.len()), generation_ };
        (void)uses_by_version_.insert(version, uses_.len());
        uses_.push({ version, result });
        return Ok(result);
    }
    auto version(resource::TextureUseHandle handle) const -> Option<fg::ResourceVersion> {
        if (handle.generation != generation_) return None();
        if (handle.index >= rstd::as_cast<u64>(uses_.len())) return None();
        return Some(fg::ResourceVersion(uses_[usize(handle.index.to_primitive())].version));
    }
    auto declare(fg::FrameGraphBuilder& graph, TextureSpec spec)
        -> Result<fg::ResourceVersion, fg::Error> {
        if (graph.data().scope != generation_.to_primitive())
            return Err(fg::Error { fg::ErrorKind::Conflict });
        if (! spec.definition.valid()) return Err(fg::Error { fg::ErrorKind::MissingResource });
        for (const auto& existing : textures_) {
            if (existing.spec.definition.id() != spec.definition.id()) continue;
            if (existing.spec.definition.revision() != spec.definition.revision() ||
                existing.spec.imported != spec.imported ||
                existing.spec.metadata != spec.metadata || existing.spec.source != spec.source ||
                existing.spec.allocation_family != spec.allocation_family ||
                existing.spec.initialize_history_transparent !=
                    spec.initialize_history_transparent ||
                ! resource::SameTextureRequest(existing.spec.request(), spec.request()))
                return Err(fg::Error { fg::ErrorKind::Conflict, {}, existing.id });
            return Ok(fg::ResourceVersion { existing.id, 0 });
        }
        auto           id = graph.next_resource_id();
        fg::Serial     allocation_class {};
        if (spec.allocation_family &&
            spec.request().lifetime == resource::TextureLifetimeClass::FrameLocal) {
            for (const auto& entry : textures_) {
                const auto& other = entry.spec;
                if (other.allocation_family == spec.allocation_family && other.definition.valid() &&
                    other.request().kind == spec.request().kind &&
                    other.request().definition == spec.request().definition &&
                    other.request().lifetime == spec.request().lifetime &&
                    other.request().content == spec.request().content) {
                    allocation_class = graph.find_resource(entry.id)->allocation_class;
                    break;
                }
            }
            if (! allocation_class) allocation_class = id.value;
        }
        fg::ResourceDescription description {
            .id               = id,
            .spec             = id.value,
            .allocation_class = allocation_class,
            .storage          = spec.allocation_family ? 0 : id.value,
            .retained = spec.request().lifetime != resource::TextureLifetimeClass::FrameLocal,
            .name     = spec.key.clone()
        };
        if (spec.request().definition) {
            auto mips              = spec.request().definition->mip_levels.to_primitive();
            description.mip_levels = mips ? mips : 1;
            if (resource::HasTextureUsage(spec.request().definition->usage,
                                          resource::TextureUsage::DepthAttachment))
                description.aspects = 2;
        }
        if (spec.metadata) {
            description.mip_levels = spec.metadata->mip_levels;
            description.layers     = spec.metadata->layers;
            description.aspects    = vrento::texture::format_layout(spec.metadata->format).aspect ==
                                             vrento::texture::Aspect::Depth
                                         ? 2
                                         : 1;
        }
        auto declared = spec.imported ? graph.import_resource(rstd::move(description))
                                      : graph.create_resource(rstd::move(description));
        if (declared.is_err()) return declared;
        (void)textures_by_id_.insert(id, textures_.len());
        textures_.push({ id, rstd::move(spec) });
        return declared;
    }
    auto bindings(const fg::FrameGraph& graph, const resource::ResourcePlan& plan) const
        -> Result<GraphResourceMap, fg::Error> {
        if (plan.generation != generation_) return Err(fg::Error { fg::ErrorKind::Conflict });
        if (graph.scope() != generation_.to_primitive())
            return Err(fg::Error { fg::ErrorKind::Conflict });
        HashMap<resource::TextureUseHandle, usize> texture_entries;
        HashMap<resource::BufferUseHandle, usize>  buffer_entries;
        for (usize i {}; i < plan.textures.len(); ++i)
            if (texture_entries.insert(plan.textures[i].handle, i))
                return Err(fg::Error { fg::ErrorKind::Duplicate });
        for (usize i {}; i < plan.buffers.len(); ++i)
            if (buffer_entries.insert(plan.buffers[i].handle, i))
                return Err(fg::Error { fg::ErrorKind::Duplicate });
        GraphResourceMap result { .scope = graph.scope() };
        const auto&      indexed = graph.inspect();
        for (usize i {}; i < indexed.accesses.len(); ++i) {
            const auto& access = indexed.accesses[i];
            if (! access.used()) continue;
            const auto& life    = indexed.lifetimes[i];
            auto        version = life.version;
            auto        missing = [&] {
                auto pass = graph.passes()[usize(*life.uses.first().unwrap())].id;
                return Err(fg::Error { fg::ErrorKind::MissingResource, pass, version.resource });
            };
            if (auto index = uses_by_version_.get(version)) {
                const auto& texture = uses_[**index];
                auto        entry   = texture_entries.get(texture.handle);
                if (! entry || ! plan.textures[**entry].definition.valid()) return missing();
                result.textures.push(
                    { texture, access.bounds, plan.textures[**entry].definition.clone() });
            } else if (auto index = buffers_by_id_.get(version.resource)) {
                auto use   = buffers_[**index].use;
                auto entry = buffer_entries.get(use);
                if (! entry || ! plan.buffers[**entry].definition.valid()) return missing();
                result.buffers.push(
                    { { use, version }, access.bounds, plan.buffers[**entry].definition.clone() });
            } else {
                auto resource = graph.find_resource(version.resource);
                if (! resource || ! resource->imported) return missing();
                result.externals.push(fg::ResourceVersion(version));
            }
        }
        return Ok(rstd::move(result));
    }
    auto resourcePlan(const fg::FrameGraph& graph) const
        -> Result<resource::ResourcePlan, fg::Error> {
        if (graph.scope() != generation_.to_primitive())
            return Err(fg::Error { fg::ErrorKind::Conflict });
        resource::ResourcePlan result { .generation = generation_ };
        for (const auto& use : uses_) {
            const auto* texture  = find(use.version.resource);
            const auto* version  = graph.find_version(use.version);
            const auto* resource = graph.find_resource(use.version.resource);
            if (! texture || ! texture->spec.definition.valid() || ! version || ! resource)
                return Err(fg::Error { fg::ErrorKind::MissingResource, {}, use.version.resource });
            const auto& request  = texture->spec.request();
            const bool  preserve = resource->imported && ! texture->spec.imported;
            const bool  initialize =
                preserve && texture->spec.initialize_history_transparent &&
                request.kind == resource::TextureRequestKind::RenderTarget && request.definition &&
                ! resource::HasTextureUsage(request.definition->usage,
                                            resource::TextureUsage::DepthAttachment) &&
                graph.version_access({ texture->id, 0 }).read &&
                graph.has_resource_writes(texture->id);
            String key;
            if (! preserve && texture->spec.allocation_family &&
                request.lifetime == resource::TextureLifetimeClass::FrameLocal) {
                auto slot = graph.allocation_slot(use.version);
                if (slot != fg::invalid_index)
                    key = rstd::format("{}::{}::slot_{}",
                                       generation_,
                                       texture->spec.allocation_family->as_str(),
                                       slot);
            }
            bool read = graph.version_access(use.version).read;
            result.textures.push({ .handle         = use.handle,
                                   .definition     = texture->spec.definition.clone(),
                                   .allocation_key = rstd::move(key),
                                   .access  = version->writer
                                                  ? (read ? resource::ResourceAccess::ReadWrite
                                                          : resource::ResourceAccess::Write)
                                                  : resource::ResourceAccess::Read,
                                   .version = u32(use.version.version),
                                   .source  = texture->spec.source,
                                   .preserve_contents      = preserve,
                                   .initialize_transparent = initialize });
        }
        return Ok(rstd::move(result));
    }
};
} // namespace vrento::pass
