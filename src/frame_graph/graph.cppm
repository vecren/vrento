export module vrento:frame_graph.graph;
export import :frame_graph.types;
import rstd;
import ligraph;

using rstd::collections::HashMap;
using rstd::collections::HashSet;

export namespace vrento::fg
{
struct AnalysisStatistics {
    Serial attempts {}, failures {};
};
namespace frame_graph_detail
{
struct Lookup {
    HashMap<u64, Index>             resources;
    HashMap<PassId, Index>          passes;
    HashMap<ResourceVersion, Index> versions;
    Vec<Index>                      latest, output_ordinals, output_counts;
    Serial                          largest_resource {}, largest_pass {};

    auto resource(const FrameGraphData& data, ResourceId id) const -> Index {
        if (id.value && id.value <= data.resources.len().to_primitive() &&
            data.resources[usize(id.value - 1)].id == id)
            return Index(id.value - 1);
        auto found = resources.get(u64(id.value));
        return found && data.resources[usize(**found)].id == id ? **found : invalid_index;
    }
    auto pass(const FrameGraphData& data, PassId id) const -> Index {
        if (id.value && id.value <= data.passes.len().to_primitive() &&
            data.passes[usize(id.value - 1)].id == id)
            return Index(id.value - 1);
        auto found = passes.get(id);
        return found ? **found : invalid_index;
    }
    auto version(ResourceVersion id) const -> Index {
        auto found = versions.get(id);
        return found ? **found : invalid_index;
    }
    void add_resource(ResourceId id, Index index) {
        (void)resources.insert(u64(id.value), Index(index));
        latest.push(Index {});
        largest_resource = rstd::cmp::max(largest_resource, id.value);
    }
    void add_pass(PassId id, Index index) {
        (void)passes.insert(id, Index(index));
        output_counts.push(Index {});
        largest_pass = rstd::cmp::max(largest_pass, id.value);
    }
    void add_version(const FrameGraphData& data, const VersionNode& node, Index index) {
        (void)versions.insert(node.id, Index(index));
        auto r = resource(data, node.id.resource);
        if (r != invalid_index)
            latest[usize(r)] = rstd::cmp::max(latest[usize(r)], node.id.version);
        auto writer = node.writer ? pass(data, *node.writer) : invalid_index;
        output_ordinals.push(writer == invalid_index ? invalid_index
                                                     : output_counts[usize(writer)]++);
    }
    static auto build(const FrameGraphData& data) -> Lookup {
        Lookup result;
        for (Index i {}; i < data.resources.len().to_primitive(); ++i)
            result.add_resource(data.resources[usize(i)].id, i);
        for (Index i {}; i < data.passes.len().to_primitive(); ++i)
            result.add_pass(data.passes[usize(i)].id, i);
        for (Index i {}; i < data.versions.len().to_primitive(); ++i)
            result.add_version(data, data.versions[usize(i)], i);
        return result;
    }
};
} // namespace frame_graph_detail

class FrameGraph {
    friend class FrameGraphBuilder;
    using Lookup = frame_graph_detail::Lookup;
    struct Data {
        FrameGraphData graph;
        FrameGraphPlan plan;
        Lookup         lookup;
        Data(FrameGraphData input, FrameGraphPlan compiled, Lookup indexes)
            : graph(rstd::move(input)), plan(rstd::move(compiled)), lookup(rstd::move(indexes)) {}
    };
    rstd::sync::Arc<Data> data_;
    explicit FrameGraph(FrameGraphData graph, FrameGraphPlan plan, Lookup lookup)
        : data_(rstd::sync::Arc<Data>::make(rstd::move(graph), rstd::move(plan),
                                            rstd::move(lookup))) {}
    static auto analyze(const FrameGraphData& data, const Lookup& lookup, Limits limits,
                        bool authored) -> Result<FrameGraphPlan, Error>;
    static auto finish(FrameGraphData data, Lookup lookup, Limits limits, bool authored,
                       AnalysisStatistics* statistics) -> Result<FrameGraph, Error> {
        if (statistics) ++statistics->attempts;
        auto plan = analyze(data, lookup, limits, authored);
        if (plan.is_err()) {
            if (statistics) ++statistics->failures;
            return Err(plan.unwrap_err());
        }
        return Ok(FrameGraph(rstd::move(data), plan.unwrap(), rstd::move(lookup)));
    }

public:
    FrameGraph(const FrameGraph& other): data_(other.data_.clone()) {}
    FrameGraph(FrameGraph&&) = default;
    auto operator=(const FrameGraph& other) -> FrameGraph& {
        if (this != &other) data_ = other.data_.clone();
        return *this;
    }
    auto        operator=(FrameGraph&&) -> FrameGraph& = default;
    static auto create(FrameGraphData data, Limits limits = {},
                       AnalysisStatistics* statistics = nullptr) -> Result<FrameGraph, Error> {
        auto lookup = Lookup::build(data);
        return finish(rstd::move(data), rstd::move(lookup), limits, false, statistics);
    }
    auto find_resource(ResourceId id) const -> const ResourceDescription* {
        auto i = data_->lookup.resource(data_->graph, id);
        return i == invalid_index ? nullptr : rstd::addressof(resources()[usize(i)]);
    }
    auto find_version(ResourceVersion id) const -> const VersionNode* {
        auto i = version_index(id);
        return i == invalid_index ? nullptr : rstd::addressof(versions()[usize(i)]);
    }
    auto version_index(ResourceVersion id) const -> Index { return data_->lookup.version(id); }
    auto find_pass(PassId id) const -> const PassNode* {
        auto i = data_->lookup.pass(data_->graph, id);
        return i == invalid_index ? nullptr : rstd::addressof(passes()[usize(i)]);
    }
    auto output_ordinal(ResourceVersion id) const -> Index {
        auto i = data_->lookup.version(id);
        return i == invalid_index ? invalid_index : data_->lookup.output_ordinals[usize(i)];
    }
    auto version_access(ResourceVersion id) const -> VersionAccess {
        auto i = data_->lookup.version(id);
        return i == invalid_index ? VersionAccess {} : data_->plan.accesses[usize(i)];
    }
    auto allocation_slot(ResourceVersion id) const -> Index {
        auto i = data_->lookup.version(id);
        return i == invalid_index ? invalid_index : data_->plan.allocations[usize(i)].slot;
    }
    bool has_resource_writes(ResourceId id) const {
        auto i = data_->lookup.resource(data_->graph, id);
        return i != invalid_index && data_->plan.resource_written[usize(i)];
    }
    auto pass_slot(PassId id) const -> Index { return data_->lookup.pass(data_->graph, id); }
    auto resource_slot(ResourceId id) const -> Index {
        return data_->lookup.resource(data_->graph, id);
    }
    auto scope() const -> Serial { return data_->graph.scope; }
    auto resources() const -> slice<ResourceDescription> {
        return data_->graph.resources.as_slice();
    }
    auto versions() const -> slice<VersionNode> { return data_->graph.versions.as_slice(); }
    auto passes() const -> slice<PassNode> { return data_->graph.passes.as_slice(); }
    auto dependencies() const -> slice<Dependency> { return data_->graph.dependencies.as_slice(); }
    auto exports() const -> slice<ResourceVersion> { return data_->graph.exports.as_slice(); }
    auto inspect() const -> const FrameGraphPlan& { return data_->plan; }
    auto clone_data() const -> FrameGraphData { return data_->graph.clone(); }
    auto check_limits(Limits limits = {}) const -> Result<empty, Error> {
        if (resources().len().to_primitive() > limits.resources ||
            versions().len().to_primitive() > limits.versions ||
            passes().len().to_primitive() > limits.passes || data_->plan.use_count > limits.uses ||
            data_->plan.name_bytes > limits.name_bytes ||
            data_->plan.dependencies.len().to_primitive() > limits.dependencies)
            return Err(Error { ErrorKind::Limit });
        return Ok(empty {});
    }
    auto compile(Limits limits = {}) const -> Result<FrameGraphPlan, Error> {
        auto result = check_limits(limits);
        if (result.is_err()) return Err(result.unwrap_err());
        return Ok(data_->plan.clone());
    }
};

class FrameGraphBuilder {
    FrameGraphData data_;
    frame_graph_detail::Lookup lookup_;
    HashSet<u64>               edges_;
    HashSet<ResourceVersion>   exports_;
    Serial         next_use_ { 1 };
    bool                       authored_ { true };
    auto resource_index(ResourceId id) const -> Index { return lookup_.resource(data_, id); }
    auto pass_index(PassId id) const -> Index { return lookup_.pass(data_, id); }
    auto version_index(ResourceVersion id) const -> Index { return lookup_.version(id); }
    void append_version(VersionNode node) {
        lookup_.add_version(data_, node, Index(data_.versions.len().to_primitive()));
        data_.versions.push(rstd::move(node));
    }
    auto declare_resource(ResourceDescription desc, bool imported)
        -> Result<ResourceVersion, Error> {
        if (! desc.id.valid()) return Err(Error { ErrorKind::Invalid, {}, desc.id });
        if (lookup_.resources.get(u64(desc.id.value)))
            return Err(Error { ErrorKind::Duplicate, {}, desc.id });
        desc.imported = imported;
        ResourceVersion first { desc.id, 0 };
        lookup_.add_resource(desc.id, Index(data_.resources.len().to_primitive()));
        data_.resources.push(rstd::move(desc));
        append_version({ .id = first, .available = imported });
        return Ok(first);
    }

public:
    auto find_resource(ResourceId id) const -> const ResourceDescription* {
        auto i = resource_index(id);
        return i == invalid_index ? nullptr : rstd::addressof(data_.resources[usize(i)]);
    }
    auto find_version(ResourceVersion id) const -> const VersionNode* {
        auto i = version_index(id);
        return i == invalid_index ? nullptr : rstd::addressof(data_.versions[usize(i)]);
    }
    auto find_pass(PassId id) const -> const PassNode* {
        auto i = pass_index(id);
        return i == invalid_index ? nullptr : rstd::addressof(data_.passes[usize(i)]);
    }
    auto output_ordinal(ResourceVersion id) const -> Index {
        auto i = version_index(id);
        return i == invalid_index ? invalid_index : lookup_.output_ordinals[usize(i)];
    }
    auto next_resource_id() const -> ResourceId {
        return { lookup_.largest_resource + 1, data_.scope };
    }
    auto reserve_pass_id() -> PassId { return { ++lookup_.largest_pass }; }
    explicit FrameGraphBuilder(Serial scope): data_ { .scope = scope } {}
    explicit FrameGraphBuilder(FrameGraphData data)
        : data_(rstd::move(data)),
          lookup_(frame_graph_detail::Lookup::build(data_)),
          authored_(false) {
        for (const auto& pass : data_.passes)
            for (const auto& use : pass.uses) next_use_ = rstd::cmp::max(next_use_, use.id + 1);
        for (auto edge : data_.dependencies) {
            auto before = pass_index(edge.before), after = pass_index(edge.after);
            (void)edges_.insert(u64((Serial(before) << 32) | after));
        }
        for (auto version : data_.exports) (void)exports_.insert(version);
    }
    auto import_resource(ResourceDescription desc) -> Result<ResourceVersion, Error> {
        return declare_resource(rstd::move(desc), true);
    }
    auto create_resource(ResourceDescription desc) -> Result<ResourceVersion, Error> {
        return declare_resource(rstd::move(desc), false);
    }
    auto import_initial(ResourceId id, bool retained = true) -> Result<empty, Error> {
        auto r = resource_index(id), v = version_index({ id, 0 });
        if (r == invalid_index || v == invalid_index)
            return Err(Error { ErrorKind::MissingResource, {}, id });
        data_.resources[usize(r)].imported = true;
        data_.resources[usize(r)].retained |= retained;
        data_.versions[usize(v)].available = true;
        return Ok(empty {});
    }
    auto reserve_write(PassId pass, ResourceVersion base, bool same_storage = false,
                       bool inherits = false, bool available = true)
        -> Result<ResourceVersion, Error> {
        if (pass_index(pass) == invalid_index) return Err(Error { ErrorKind::MissingPass, pass });
        auto r = resource_index(base.resource);
        if (r == invalid_index || version_index(base) == invalid_index)
            return Err(Error { ErrorKind::MissingResource, pass, base.resource });
        auto next = lookup_.latest[usize(r)];
        if (next == invalid_index) return Err(Error { ErrorKind::Limit, pass, base.resource });
        ResourceVersion output { base.resource, next + 1 };
        append_version({ output, Some(pass), Some(base), inherits, same_storage, available });
        return Ok(output);
    }
    auto prepend_write(PassId pass, ResourceId resource) -> Result<ResourceVersion, Error> {
        if (pass_index(pass) == invalid_index) return Err(Error { ErrorKind::MissingPass, pass });
        if (resource_index(resource) == invalid_index)
            return Err(Error { ErrorKind::MissingResource, pass, resource });
        for (const auto& version : data_.versions)
            if (version.id.resource == resource && version.id.version == invalid_index)
                return Err(Error { ErrorKind::Limit, pass, resource });
        for (auto& version : data_.versions) {
            if (version.id.resource != resource) continue;
            if (version.id.version) ++version.id.version;
            if (version.previous) ++version.previous->version;
        }
        for (auto& node : data_.passes)
            for (auto& use : node.uses)
                if (use.version.resource == resource) ++use.version.version;
        for (auto& output : data_.exports)
            if (output.resource == resource) ++output.version;
        ResourceVersion output { resource, 1 };
        data_.versions.push(
            { output, Some(pass), Some(ResourceVersion { resource, 0 }), true, true, true });
        auto largest_pass    = lookup_.largest_pass;
        lookup_              = frame_graph_detail::Lookup::build(data_);
        lookup_.largest_pass = largest_pass;
        exports_.clear();
        for (auto version : data_.exports) (void)exports_.insert(version);
        return Ok(output);
    }
    auto write_range(PassId pass, ResourceVersion output, Option<ResourceRange> selected = None())
        -> Result<Serial, Error> {
        auto p = pass_index(pass), r = resource_index(output.resource), v = version_index(output);
        if (p == invalid_index) return Err(Error { ErrorKind::MissingPass, pass });
        if (r == invalid_index || v == invalid_index)
            return Err(Error { ErrorKind::MissingResource, pass, output.resource });
        if (! data_.versions[usize(v)].writer || *data_.versions[usize(v)].writer != pass)
            return Err(Error { ErrorKind::Conflict, pass, output.resource });
        auto range = selected.unwrap_or(full_range(data_.resources[usize(r)]));
        if (! valid_range(data_.resources[usize(r)], range))
            return Err(Error { ErrorKind::Range, pass, output.resource });
        auto id = next_use_++;
        data_.passes[usize(p)].uses.push({ id, output, Access::Write, range });
        return Ok(id);
    }
    auto set_contents_available(ResourceVersion output, bool available) -> Result<empty, Error> {
        auto index = version_index(output);
        if (index == invalid_index || ! data_.versions[usize(index)].writer)
            return Err(Error { ErrorKind::MissingProducer, {}, output.resource });
        data_.versions[usize(index)].available = available;
        return Ok(empty {});
    }
    auto reuse_storage(ResourceVersion output) -> Result<empty, Error> {
        auto v = version_index(output);
        if (v == invalid_index)
            return Err(Error { ErrorKind::MissingResource, {}, output.resource });
        if (! data_.versions[usize(v)].previous)
            return Err(Error { ErrorKind::Invalid, {}, output.resource });
        data_.versions[usize(v)].same_storage = true;
        return Ok(empty {});
    }
    auto add_pass(PassId id, LogicalQueueId queue = {}, String name = {}) -> Result<PassId, Error> {
        if (! id.valid()) return Err(Error { ErrorKind::Invalid, id });
        if (pass_index(id) != invalid_index) return Err(Error { ErrorKind::Duplicate, id });
        lookup_.add_pass(id, Index(data_.passes.len().to_primitive()));
        data_.passes.push({ id, queue, rstd::move(name) });
        return Ok(id);
    }
    auto read(PassId pass, ResourceVersion version, Option<ResourceRange> selected = None(),
              bool requires_contents = true) -> Result<Serial, Error> {
        auto p = pass_index(pass), r = resource_index(version.resource), v = version_index(version);
        if (p == invalid_index) return Err(Error { ErrorKind::MissingPass, pass });
        if (r == invalid_index || v == invalid_index)
            return Err(Error { ErrorKind::MissingResource, pass, version.resource });
        auto range = selected.unwrap_or(full_range(data_.resources[usize(r)]));
        if (! valid_range(data_.resources[usize(r)], range))
            return Err(Error { ErrorKind::Range, pass, version.resource });
        auto id = next_use_++;
        data_.passes[usize(p)].uses.push({ id, version, Access::Read, range, requires_contents });
        return Ok(id);
    }
    auto write(PassId pass, ResourceVersion base, Option<ResourceRange> selected = None(),
               bool same_storage = false, bool inherits = false, bool available = true)
        -> Result<ResourceVersion, Error> {
        auto p = pass_index(pass), r = resource_index(base.resource), v = version_index(base);
        if (p == invalid_index) return Err(Error { ErrorKind::MissingPass, pass });
        if (r == invalid_index || v == invalid_index)
            return Err(Error { ErrorKind::MissingResource, pass, base.resource });
        auto range = selected.unwrap_or(full_range(data_.resources[usize(r)]));
        if (! valid_range(data_.resources[usize(r)], range))
            return Err(Error { ErrorKind::Range, pass, base.resource });
        auto reserved = reserve_write(pass, base, same_storage, inherits, available);
        if (reserved.is_err()) return reserved;
        auto output = reserved.unwrap();
        write_range(pass, output, Some(range)).unwrap();
        return Ok(output);
    }
    auto depends_on(PassId pass, PassId before) -> Result<empty, Error> {
        auto from = pass_index(before), to = pass_index(pass);
        if (from == invalid_index || to == invalid_index)
            return Err(Error { ErrorKind::MissingPass, pass });
        if (edges_.insert(u64((Serial(from) << 32) | to)))
            data_.dependencies.push({ before, pass });
        return Ok(empty {});
    }
    auto export_resource(ResourceVersion version) -> Result<empty, Error> {
        if (version_index(version) == invalid_index)
            return Err(Error { ErrorKind::MissingResource, {}, version.resource });
        if (exports_.insert(version)) data_.exports.push(rstd::move(version));
        return Ok(empty {});
    }
    auto data() const -> const FrameGraphData& { return data_; }
    auto finish(Limits              limits     = {},
                AnalysisStatistics* statistics = nullptr) && -> Result<FrameGraph, Error> {
        return FrameGraph::finish(
            rstd::move(data_), rstd::move(lookup_), limits, authored_, statistics);
    }
};
} // namespace vrento::fg

namespace vrento::fg
{
auto FrameGraph::analyze(const FrameGraphData& data, const Lookup& lookup, Limits limits,
                         bool authored) -> Result<FrameGraphPlan, Error> {
    if (! data.scope) return Err(Error { ErrorKind::Invalid });
    if (data.resources.len().to_primitive() > limits.resources ||
        data.versions.len().to_primitive() > limits.versions ||
        data.passes.len().to_primitive() > limits.passes ||
        data.dependencies.len().to_primitive() > limits.dependencies)
        return Err(Error { ErrorKind::Limit });
    auto resource_index = [&](ResourceId id) {
        return lookup.resource(data, id);
    };
    auto pass_index = [&](PassId id) {
        return lookup.pass(data, id);
    };
    auto version_index = [&](ResourceVersion id) {
        return lookup.version(id);
    };
    struct AliasedUse {
        Index              pass, resource;
        const ResourceUse* use;
    };
    struct StorageGroup {
        Index      first_version { invalid_index };
        bool       aliased {};
        Vec<Index> uses;
    };
    HashMap<u64, Index> storage_indexes;
    Vec<StorageGroup>   storage_groups;
    Vec<Index>          resource_storage;
    Vec<Index>          version_resources;
    Vec<AliasedUse>     alias_uses;
    Serial              names {}, uses_count {};
    for (Index i {}; i < data.resources.len().to_primitive(); ++i) {
        const auto& r = data.resources[usize(i)];
        if (! r.id.valid() || Index(r.kind) > Index(ResourceKind::Buffer) ||
            (r.kind == ResourceKind::Buffer && ! r.bytes) ||
            (r.kind == ResourceKind::Texture &&
             (! r.mip_levels || ! r.layers || ! r.aspects || (r.aspects & ~Index(7)))))
            return Err(Error { ErrorKind::Invalid, {}, r.id });
        if (! authored && **lookup.resources.get(u64(r.id.value)) != i)
            return Err(Error { ErrorKind::Duplicate, {}, r.id });
        Index storage = invalid_index;
        if (r.storage) {
            if (auto found = storage_indexes.get(u64(r.storage)); found) {
                storage                                = **found;
                storage_groups[usize(storage)].aliased = true;
            } else {
                storage = Index(storage_groups.len().to_primitive());
                (void)storage_indexes.insert(u64(r.storage), Index(storage));
                storage_groups.push(StorageGroup {});
            }
        }
        resource_storage.push(Index(storage));
        names += r.name.len().to_primitive();
    }
    for (Index i {}; i < data.passes.len().to_primitive(); ++i) {
        const auto& p = data.passes[usize(i)];
        if (! p.id.valid()) return Err(Error { ErrorKind::Invalid, p.id });
        if (! authored && **lookup.passes.get(p.id) != i)
            return Err(Error { ErrorKind::Duplicate, p.id });
        uses_count += p.uses.len().to_primitive();
        names += p.name.len().to_primitive();
    }
    if (names > limits.name_bytes || uses_count > limits.uses)
        return Err(Error { ErrorKind::Limit });
    for (Index i {}; i < data.versions.len().to_primitive(); ++i) {
        const auto& v = data.versions[usize(i)];
        auto        r = resource_index(v.id.resource);
        if (r == invalid_index) return Err(Error { ErrorKind::MissingResource, {}, v.id.resource });
        version_resources.push(Index(r));
        if (! authored && lookup.version(v.id) != i)
            return Err(Error { ErrorKind::Duplicate, {}, v.id.resource });
        if (v.writer && pass_index(*v.writer) == invalid_index)
            return Err(Error { ErrorKind::MissingPass, *v.writer });
        if (! v.id.version && v.writer)
            return Err(Error { ErrorKind::Invalid, *v.writer, v.id.resource });
        if (v.id.version && ! v.writer)
            return Err(Error { ErrorKind::MissingProducer, {}, v.id.resource });
        if (! v.id.version && v.available != data.resources[usize(r)].imported)
            return Err(Error { ErrorKind::Invalid, {}, v.id.resource });
        if (v.previous &&
            (v.previous->resource != v.id.resource || v.previous->version >= v.id.version))
            return Err(Error { ErrorKind::Invalid, {}, v.id.resource });
    }
    for (const auto& v : data.versions)
        if (v.previous && version_index(*v.previous) == invalid_index)
            return Err(Error { ErrorKind::MissingProducer, {}, v.id.resource });
    struct StorageAccess {
        Index         pass;
        ResourceRange range;
    };
    FrameGraphPlan          plan { .use_count = uses_count, .name_bytes = names };
    Vec<Vec<ResourceRange>> writes;
    Vec<Vec<StorageAccess>> reads;
    Vec<Vec<Index>>         version_uses;
    plan.accesses.resize(data.versions.len(), VersionAccess {});
    plan.resource_written.resize(data.resources.len(), false);
    for (usize i {}; i < data.versions.len(); ++i) {
        writes.push(Vec<ResourceRange> {});
        reads.push(Vec<StorageAccess> {});
        version_uses.push(Vec<Index> {});
    }
    HashSet<u64> use_ids;
    for (Index pass {}; pass < data.passes.len().to_primitive(); ++pass) {
        const auto& p = data.passes[usize(pass)];
        for (const auto& use : p.uses) {
            auto r = resource_index(use.version.resource), v = version_index(use.version);
            if (! authored && (! use.id || ! use_ids.insert(u64(use.id))))
                return Err(Error { ErrorKind::Duplicate, p.id, use.version.resource });
            if (r == invalid_index || v == invalid_index)
                return Err(Error { ErrorKind::MissingResource, p.id, use.version.resource });
            if (Index(use.access) > Index(Access::Write))
                return Err(Error { ErrorKind::Invalid, p.id, use.version.resource });
            if (! valid_range(data.resources[usize(r)], use.range))
                return Err(Error { ErrorKind::Range, p.id, use.version.resource });
            auto& access = plan.accesses[usize(v)];
            if (! access.used()) {
                access.bounds = use.range;
            } else {
                auto& bound   = access.bounds;
                auto  include = [](auto& start, auto& count, auto other_start, auto other_count) {
                    auto end = rstd::cmp::max(start + count, other_start + other_count);
                    start    = rstd::cmp::min(start, other_start);
                    count    = end - start;
                };
                bound.aspect |= use.range.aspect;
                include(bound.offset, bound.size, use.range.offset, use.range.size);
                include(bound.first_mip, bound.mip_count, use.range.first_mip, use.range.mip_count);
                include(bound.first_layer,
                        bound.layer_count,
                        use.range.first_layer,
                        use.range.layer_count);
            }
            auto& consumers = version_uses[usize(v)];
            if (consumers.is_empty() || *consumers.last().unwrap() != pass)
                consumers.push(Index(pass));
            if (use.access == Access::Write) {
                const auto& node = data.versions[usize(v)];
                if (! node.writer || *node.writer != p.id)
                    return Err(Error { ErrorKind::Conflict, p.id, use.version.resource });
                writes[usize(v)].push(ResourceRange(use.range));
                access.written                  = true;
                plan.resource_written[usize(r)] = true;
            } else {
                reads[usize(v)].push({ pass, use.range });
                access.read = true;
            }
            const auto storage = resource_storage[usize(r)];
            if (storage != invalid_index && storage_groups[usize(storage)].aliased) {
                storage_groups[usize(storage)].uses.push(Index(alias_uses.len().to_primitive()));
                alias_uses.push({ pass, r, rstd::addressof(use) });
            }
        }
    }
    for (usize i {}; i < data.versions.len(); ++i)
        if (data.versions[i].writer && writes[i].is_empty())
            return Err(Error { ErrorKind::MissingProducer,
                               *data.versions[i].writer,
                               data.versions[i].id.resource });
    auto available = [&](ResourceVersion requested, ResourceRange range) -> bool {
        Vec<ResourceRange> uncovered;
        uncovered.push(rstd::move(range));
        auto v = version_index(requested);
        while (v != invalid_index) {
            const auto& node = data.versions[usize(v)];
            if (! node.writer)
                return data.resources[usize(resource_index(node.id.resource))].imported;
            auto kind = data.resources[usize(resource_index(node.id.resource))].kind;
            for (auto written : writes[usize(v)]) {
                Vec<ResourceRange> remainder;
                for (auto part : uncovered) {
                    if (! node.available && overlaps(kind, part, written)) return false;
                    auto pieces = subtract_range(kind, part, written);
                    remainder.extend_from_slice(pieces.as_slice());
                }
                uncovered = rstd::move(remainder);
            }
            if (uncovered.is_empty()) return true;
            if (! node.inherits || ! node.previous) return false;
            v = version_index(*node.previous);
        }
        return false;
    };
    Vec<Vec<Index>> predecessors;
    for (usize i {}; i < data.passes.len(); ++i) {
        plan.successors.push(Vec<Index> {});
        predecessors.push(Vec<Index> {});
    }
    HashMap<u64, Index> edges;
    auto                edge = [&](Index before, Index after, DependencySource source) -> bool {
        auto key      = u64((Serial(before) << 32) | after);
        auto existing = edges.get(key);
        if (existing) {
            plan.dependencies[usize(**existing)].sources |= Index(source);
            return true;
        }
        (void)edges.insert(key, Index(plan.dependencies.len().to_primitive()));
        if (edges.len().to_primitive() > limits.dependencies) return false;
        plan.dependencies.push({ before, after, Index(source) });
        plan.successors[usize(before)].push(Index(after));
        predecessors[usize(after)].push(Index(before));
        return true;
    };
    for (auto d : data.dependencies) {
        auto before = pass_index(d.before), after = pass_index(d.after);
        if (before == invalid_index || after == invalid_index)
            return Err(Error { ErrorKind::MissingPass });
        if (! edge(before, after, DependencySource::Explicit))
            return Err(Error { ErrorKind::Limit });
    }
    Vec<Index> roots;
    for (Index i {}; i < data.versions.len().to_primitive(); ++i) roots.push(Index(i));
    auto root = [&](Index i) {
        while (roots[usize(i)] != i) i = roots[usize(i)];
        return i;
    };
    for (Index i {}; i < data.versions.len().to_primitive(); ++i) {
        const auto& v       = data.versions[usize(i)];
        const auto  storage = resource_storage[usize(version_resources[usize(i)])];
        if (v.previous && (v.same_storage || storage != invalid_index))
            roots[usize(root(i))] = root(version_index(*v.previous));
        if (storage != invalid_index) {
            auto& first = storage_groups[usize(storage)].first_version;
            if (first == invalid_index)
                first = i;
            else
                roots[usize(root(i))] = root(first);
        }
        if (v.inherits && v.previous) {
            const auto& previous = data.versions[usize(version_index(*v.previous))];
            if (previous.writer && *previous.writer != *v.writer &&
                ! edge(pass_index(*previous.writer),
                       pass_index(*v.writer),
                       DependencySource::Contents))
                return Err(Error { ErrorKind::Limit });
        }
    }
    for (Index p {}; p < data.passes.len().to_primitive(); ++p)
        for (const auto& use : data.passes[usize(p)].uses) {
            auto        v    = version_index(use.version);
            const auto& node = data.versions[usize(v)];
            if (use.access == Access::Read) {
                if (use.requires_contents && ! available(use.version, use.range))
                    return Err(Error {
                        ErrorKind::Undefined, data.passes[usize(p)].id, use.version.resource });
                if (node.writer) {
                    auto before = pass_index(*node.writer);
                    if (before == p)
                        return Err(Error {
                            ErrorKind::Conflict, data.passes[usize(p)].id, use.version.resource });
                    if (! edge(before, p, DependencySource::Contents))
                        return Err(Error { ErrorKind::Limit });
                }
            }
        }

    HashMap<u64, Vec<StorageAccess>> frontiers;
    Vec<Index>                       ordered_versions;
    for (Index v {}; v < data.versions.len().to_primitive(); ++v) ordered_versions.push(Index(v));
    rstd::slice_::sort_unstable_by(ordered_versions.as_mut_slice().as_mut_ref(),
                                   [&](Index a, Index b) {
                                       const auto& left  = data.versions[usize(a)].id;
                                       const auto& right = data.versions[usize(b)].id;
                                       if (left.resource.value != right.resource.value)
                                           return left.resource.value < right.resource.value;
                                       return left.version < right.version;
                                   });
    for (auto v : ordered_versions) {
        const auto& node  = data.versions[usize(v)];
        auto        r     = resource_index(node.id.resource);
        auto        kind  = data.resources[usize(r)].kind;
        auto        key   = u64((Serial(r) << 32) | root(v));
        auto        found = frontiers.get_mut(key);
        if (found.is_none()) {
            (void)frontiers.insert(u64(key), Vec<StorageAccess> {});
            found = frontiers.get_mut(key);
        }
        auto& frontier = **found;
        if (node.writer) {
            auto p = pass_index(*node.writer);
            for (auto written : writes[usize(v)]) {
                Vec<StorageAccess> remaining;
                for (const auto& prior : frontier) {
                    if (p != prior.pass && overlaps(kind, prior.range, written) &&
                        ! edge(prior.pass, p, DependencySource::Storage))
                        return Err(Error { ErrorKind::Limit });
                    // The new writer orders covered accesses transitively; keep uncovered
                    // ranges so partial writes cannot hide outstanding storage hazards.
                    auto pieces = subtract_range(kind, prior.range, written);
                    for (auto range : pieces) remaining.push({ prior.pass, range });
                }
                remaining.push({ p, written });
                frontier = rstd::move(remaining);
            }
        }
        frontier.extend_from_slice(reads[usize(v)].as_slice());
    }
    Vec<bool> retained_versions;
    retained_versions.resize(data.versions.len(), false);
    for (auto exported : data.exports) {
        auto r = resource_index(exported.resource), v = version_index(exported);
        if (r == invalid_index || v == invalid_index)
            return Err(Error { ErrorKind::MissingResource, {}, exported.resource });
        if (! available(exported, full_range(data.resources[usize(r)])))
            return Err(Error { ErrorKind::Undefined, {}, exported.resource });
        retained_versions[usize(v)] = true;
    }
    Vec<Index> positions, batches;
    for (Index i {}; i < data.passes.len().to_primitive(); ++i) {
        positions.push(Index(invalid_index));
        batches.push(Index(invalid_index));
    }
    auto topology = ligraph::indexed_view(plan.successors.len(), [&plan](usize node) {
        return plan.successors[node].iter().map([](auto index) {
            return usize(*index);
        });
    });
    ligraph::TopoWorkspace ordering;
    auto                   sorted = ligraph::toposort_min(topology, ordering);
    if (sorted.is_err())
        return Err(Error { sorted.unwrap_err().kind == ligraph::ErrorKind::Cycle
                               ? ErrorKind::Cycle
                               : ErrorKind::Invalid });
    for (auto node : *sorted) plan.order.push(Index(node.to_primitive()));
    ligraph::SearchWorkspace reachability;
    HashMap<u32, Index>      last_queue;
    for (usize position {}; position < plan.order.len(); ++position) {
        auto p              = plan.order[position];
        positions[usize(p)] = Index(position.to_primitive());
        auto previous       = last_queue.get(u32(data.passes[usize(p)].queue.value));
        if (previous && ! edge(**previous, p, DependencySource::Queue))
            return Err(Error { ErrorKind::Limit });
        (void)last_queue.insert(u32(data.passes[usize(p)].queue.value), Index(p));
        ExecutionBatch batch { data.passes[usize(p)].queue };
        batch.passes.push(Index(p));
        for (auto before : predecessors[usize(p)]) batch.waits.push(Index(batches[usize(before)]));
        batches[usize(p)] = Index(plan.batches.len().to_primitive());
        plan.batches.push(rstd::move(batch));
    }
    for (const auto& entry : alias_uses) {
        const auto& use      = *entry.use;
        const auto& resource = data.resources[usize(entry.resource)];
        const auto& group    = storage_groups[usize(resource_storage[usize(entry.resource)])];
        for (const auto index : group.uses) {
            const auto& other = alias_uses[usize(index)];
            // Include every use in the same pass, regardless of its declaration order.
            if (other.pass > entry.pass) break;
            if (entry.resource == other.resource ||
                (use.access == Access::Read && other.use->access == Access::Read))
                continue;
            if (resource.kind != data.resources[usize(other.resource)].kind ||
                (overlaps(resource.kind, use.range, other.use->range) &&
                 ! plan.precedes(entry.pass, other.pass, reachability) &&
                 ! plan.precedes(other.pass, entry.pass, reachability)))
                return Err(Error {
                    ErrorKind::Conflict, data.passes[usize(entry.pass)].id, use.version.resource });
        }
    }
    for (Index i {}; i < data.versions.len().to_primitive(); ++i) {
        const auto&     node = data.versions[usize(i)];
        const auto&     r    = data.resources[usize(resource_index(node.id.resource))];
        VersionLifetime life { .version = node.id,
                               .retained =
                                   r.imported || r.retained || retained_versions[usize(i)] };
        for (auto p : version_uses[usize(i)]) {
            life.uses.push(Index(p));
            life.first = rstd::cmp::min(life.first, positions[usize(p)]);
            life.last  = rstd::cmp::max(life.last, positions[usize(p)]);
        }
        plan.lifetimes.push(rstd::move(life));
    }
    struct Group {
        Index      slot;
        Serial     allocation_class;
        Vec<Index> uses;
        bool       retained {};
    };
    Vec<Group> groups;
    Vec<Index> groups_by_root;
    groups_by_root.resize(data.versions.len(), invalid_index);
    for (auto p : plan.order)
        for (const auto& use : data.passes[usize(p)].uses) {
            auto  v = version_index(use.version), owner = root(v);
            auto& group = groups_by_root[usize(owner)];
            if (group == invalid_index) {
                const auto& r = data.resources[usize(resource_index(use.version.resource))];
                group         = Index(groups.len().to_primitive());
                groups.push({ invalid_index, r.allocation_class, {} });
            }
            auto& value = groups[usize(group)];
            value.retained |= plan.lifetimes[usize(v)].retained;
            if (value.uses.is_empty() || *value.uses.last().unwrap() != p)
                value.uses.push(Index(p));
        }
    Index next_slot {};
    HashMap<u64, Vec<Index>> reusable;
    for (usize i {}; i < groups.len(); ++i) {
        auto& group = groups[i];
        if (group.retained || ! group.allocation_class) {
            group.slot = next_slot++;
        } else {
            auto key = u64(group.allocation_class);
            if (! reusable.get(key)) (void)reusable.insert(key, Vec<Index> {});
            auto& tails = **reusable.get_mut(key);
            for (auto& tail : tails) {
                const auto& previous = groups[usize(tail)];
                bool safe = true;
                // Each earlier occupant of this slot precedes its tail transitively.
                for (auto before : previous.uses) {
                    for (auto after : group.uses)
                        if (! plan.precedes(before, after, reachability)) {
                            safe = false;
                            break;
                        }
                    if (! safe) break;
                }
                if (safe) {
                    group.slot = previous.slot;
                    tail       = Index(i.to_primitive());
                    break;
                }
            }
            if (group.slot == invalid_index) {
                group.slot = next_slot++;
                tails.push(Index(i.to_primitive()));
            }
        }
    }
    for (Index v {}; v < data.versions.len().to_primitive(); ++v) {
        auto group = groups_by_root[usize(root(v))];
        auto slot  = group == invalid_index ? next_slot++ : groups[usize(group)].slot;
        plan.allocations.push({ data.versions[usize(v)].id, slot });
    }
    return Ok(rstd::move(plan));
}
} // namespace vrento::fg
