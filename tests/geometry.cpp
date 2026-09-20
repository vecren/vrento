#include <rstd/macro.hpp>

import rstd;
import rstd.cppstd;
import vrento.geometry;

using namespace rstd::prelude;
using namespace vrento;

int main() {
    VertexArray vertices({ { "position", VertexType::FLOAT3, false } }, usize(2));
    rstd_assert(vertices.ID() == u32::MAX);
    auto generation = vertices.DataGeneration();
    auto written    = vertices.RewriteVertices([](VertexWriter& writer) {
        auto first = writer.AppendZeroedVertex();
        rstd_assert(first.is_some());
        (*first)[usize()] = 1.0f;
        auto second       = writer.AppendZeroedVertex();
        rstd_assert(second.is_some());
        (*second)[usize()] = 2.0f;
        rstd_assert(writer.AppendZeroedVertex().is_none());
    });
    rstd_assert(written.vertex_count == usize(2) && written.overflowed);
    rstd_assert(vertices.DataGeneration() == generation + u64(1));
    rstd_assert(vertices.VertexCount() == usize(2));
    rstd_assert(vertices.Data()[3] == 2.0f);
    const float malformed[] { 1.0f, 2.0f };
    generation = vertices.DataGeneration();
    rstd_assert(
        ! vertices.SetVertex("position", slice<float>::from_raw_parts(malformed, usize(2))));
    rstd_assert(vertices.DataGeneration() == generation);
    rstd_assert(! vertices.SetVertexs(usize::MAX, {}));
    auto*       data = vertices.Data();
    VertexArray moved(rstd::move(vertices));
    rstd_assert(vertices.BufferView().bytes.is_empty());
    rstd_assert(vertices.BufferView().count == usize());
    rstd_assert(moved.Data() == data);
    rstd_assert(moved.DataGeneration() == generation);
    moved.ResetSize();
    rstd_assert(moved.VertexCount() == usize());
    rstd_assert(moved.CapacitySize() == usize(6));

    VertexArray empty({}, usize(2));
    auto        result = empty.RewriteVertices([](VertexWriter& writer) {
        rstd_assert(writer.AppendZeroedVertex().is_none());
    });
    rstd_assert(result.vertex_count == usize() && result.overflowed);

    IndexArray           indices(usize(3));
    const rstd::uint32_t input[] { 0, 1, 0 };
    auto                 view = slice<rstd::uint32_t>::from_raw_parts(input, usize(3));
    indices.Assign(usize(), view);
    rstd_assert(indices.DataCount() == usize(3));
    rstd_assert(indices.RenderDataCount() == usize(3));
    indices.SetRenderDataCount(usize(2));
    rstd_assert(indices.RenderDataCount() == usize(2));
    generation = indices.DataGeneration();
    indices.Assign(usize::MAX, view);
    rstd_assert(indices.DataGeneration() == generation);
    IndexArray moved_indices(rstd::move(indices));
    rstd_assert(indices.BufferView().bytes.is_empty());
    rstd_assert(indices.BufferView().count == usize());
    rstd_assert(moved_indices.Data()[1] == 1);
    rstd_assert(moved_indices.RenderDataCount() == usize(2));
}
