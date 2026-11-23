export module vrento:vulkan.pass.graph_input;
export import :vulkan.pass.input;
export import :pass.resources;
import rstd;
export namespace vrento::vulkan
{
class GraphInput {
    struct Data {
        fg::FrameGraph              graph;
        Vec<PassInput>              descriptions;
        pass::GraphResourceMap      resources;
    };
    Arc<Data> data_;
    explicit GraphInput(Arc<Data> data): data_(rstd::move(data)) {}
public:
    GraphInput(const GraphInput& other): data_(other.data_.clone()) {}
    GraphInput(GraphInput&&)                           = default;
    auto        operator=(GraphInput&&) -> GraphInput& = default;
    static auto create(fg::FrameGraph graph, Vec<PassInput> descriptions,
                       pass::GraphResourceMap resources) -> Result<GraphInput, GraphError> {
        if (resources.scope != graph.scope() || descriptions.len() != graph.passes().len())
            return Err(GraphError { GraphErrorKind::Conflict });
        return Ok(GraphInput(Arc<Data>::make(
            Data { rstd::move(graph), rstd::move(descriptions), rstd::move(resources) })));
    }
    auto graph() const -> const fg::FrameGraph& { return data_->graph; }
    auto descriptions() const -> slice<PassInput> { return data_->descriptions.as_slice(); }
    auto resources() const -> const pass::GraphResourceMap& { return data_->resources; }
};
} // namespace vrento::vulkan
