export module vrento:pass.graph_builder;
export import :pass.desc;
import :frame_graph.graph;
import rstd;
export namespace vrento::pass
{
class PassIoWriter {
    fg::FrameGraphBuilder* graph_;
    fg::PassId             id_;

    PassIoWriter(fg::FrameGraphBuilder& graph, fg::PassId id): graph_(&graph), id_(id) {}

public:
    static auto begin(fg::FrameGraphBuilder& graph, fg::PassId id, fg::LogicalQueueId queue,
                      String name = {}) -> Result<PassIoWriter, fg::Error> {
        auto existing = graph.find_pass(id);
        if (existing && (! existing->uses.is_empty() || existing->queue != queue))
            return Err(fg::Error { fg::ErrorKind::Conflict, id });
        if (! existing) {
            auto added = graph.add_pass(id, queue, rstd::move(name));
            if (added.is_err()) return Err(added.unwrap_err());
        }
        return Ok(PassIoWriter(graph, id));
    }

    auto read(const ContentRef& content, fg::ResourceRange range, bool requires_contents)
        -> Result<fg::Serial, fg::Error> {
        auto source = content.resolve(graph_->data().scope);
        if (source.is_err()) return Err(source.unwrap_err());
        return graph_->read(id_, *source, Some(range), requires_contents);
    }

    auto write(const ContentRef& content, fg::ResourceRange range, const ContentRef* reuse,
               bool contents_available) -> Result<fg::Serial, fg::Error> {
        auto target = content.resolve(graph_->data().scope);
        if (target.is_err()) return Err(target.unwrap_err());
        if (auto reference = content.output(); reference && reference->producer != id_)
            return Err(fg::Error { fg::ErrorKind::Conflict, id_, target->resource });
        if (reuse) {
            auto version = graph_->find_version(*target);
            if (! version || ! version->previous || *version->previous != reuse->version())
                return Err(fg::Error { fg::ErrorKind::Conflict, id_, target->resource });
            auto reused = graph_->reuse_storage(*target);
            if (reused.is_err()) return Err(reused.unwrap_err());
        }
        auto available = graph_->set_contents_available(*target, contents_available);
        if (available.is_err()) return Err(available.unwrap_err());
        return graph_->write_range(id_, *target, Some(range));
    }
};

inline auto declare_io(fg::FrameGraphBuilder& graph, fg::PassId id, fg::LogicalQueueId queue,
                       const PassIO& io, String name = {}) -> Result<Vec<fg::Serial>, fg::Error> {
    auto writer = PassIoWriter::begin(graph, id, queue, rstd::move(name));
    if (writer.is_err()) return Err(writer.unwrap_err());
    auto uses = Vec<fg::Serial>::with_capacity(io.inputs.len() + io.outputs.len());
    for (const auto& input : io.inputs) {
        auto result = writer->read(input.source, input.range, input.requires_contents);
        if (result.is_err()) return Err(result.unwrap_err());
        uses.push(result.unwrap());
    }
    for (const auto& output : io.outputs) {
        auto result = writer->write(output.target,
                                    output.range,
                                    output.reuse ? rstd::addressof(*output.reuse) : nullptr,
                                    output.available);
        if (result.is_err()) return Err(result.unwrap_err());
        uses.push(result.unwrap());
    }
    return Ok(rstd::move(uses));
}
} // namespace vrento::pass
