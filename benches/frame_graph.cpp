import rstd;
import vrento;

using namespace rstd::prelude;
using namespace vrento::fg;

namespace
{
constexpr usize frames(24);

auto elapsed(rstd::time::Instant start) -> u64 {
    return u64(start.elapsed().as_nanos().to_primitive());
}

auto measure(Index size) {
    array<u64, 24> declaration {};
    array<u64, 24> analysis {};
    auto           random = u32(42);
    Serial         uses {}, dependencies {};
    for (usize frame {}; frame < frames; ++frame) {
        auto                      begin = rstd::time::Instant::now();
        auto                      count = size + Index(frame.to_primitive() % 8);
        auto                      scope = Serial(frame.to_primitive() + 1);
        FrameGraphBuilder         builder(scope);
        array<ResourceVersion, 8> recent {};
        for (Index i {}; i < count; ++i) {
            auto id     = Serial(i) + 1;
            auto pass   = builder.add_pass({ id }).unwrap();
            auto target = builder.create_resource({ .id = { id, scope }, .storage = id }).unwrap();
            random      = random.wrapping_mul(u32(1664525)).wrapping_add(u32(1013904223));
            auto pick   = usize((random >> u64(16)).to_primitive() % 8);
            if (recent[pick].resource.valid()) builder.read(pass, recent[pick]).unwrap();
            recent[usize(i % 8)] = builder.write(pass, target).unwrap();
        }
        auto declared      = rstd::time::Instant::now();
        auto graph         = rstd::move(builder).finish().unwrap();
        analysis[frame]    = elapsed(declared);
        declaration[frame] = u64(declared.duration_since(begin).as_nanos().to_primitive());
        if (graph.inspect().order.len() != usize(count)) return false;
        uses += graph.inspect().use_count;
        dependencies += graph.inspect().dependencies.len().to_primitive();
    }
    auto sort = [](auto& samples) {
        for (usize i(1); i < frames; ++i)
            for (auto j = i; j > usize() && samples[j] < samples[j - usize(1)]; --j)
                rstd::mem::swap(samples[j], samples[j - usize(1)]);
    };
    sort(declaration);
    sort(analysis);
    rstd::io::println(
        "passes={} uses={} edges={} declare_ns p50={} p95={} analyze_ns p50={} p95={}",
        size,
        uses / frames.to_primitive(),
        dependencies / frames.to_primitive(),
        declaration[usize(12)],
        declaration[usize(22)],
        analysis[usize(12)],
        analysis[usize(22)]);
    return true;
}
} // namespace

int main() {
    for (auto size : array<Index, 4> { 128u, 256u, 512u, 1024u })
        if (! measure(size)) return 1;
    return 0;
}
