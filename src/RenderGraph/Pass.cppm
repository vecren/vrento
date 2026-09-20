export module vrento.rgraph:pass;
import rstd;

using namespace rstd::prelude;

export namespace vrento::rg
{

struct PassHandle {
    usize index { usize::MAX };

    bool valid() const noexcept { return index != usize::MAX; }

    friend auto operator<=>(const PassHandle&, const PassHandle&) = default;
};

struct Pass {
    Pass()          = default;
    virtual ~Pass() = default;

    Pass(const Pass&)            = delete;
    Pass& operator=(const Pass&) = delete;
};

struct VirtualPass : Pass {
    struct Desc {};

    explicit VirtualPass(Desc&&) noexcept {}
    ~VirtualPass() noexcept override = default;
};

} // namespace vrento::rg

export namespace rstd
{

template<>
struct Impl<hash::Hash, vrento::rg::PassHandle> : ImplBase<vrento::rg::PassHandle> {
    template<typename H>
        requires Impled<H, hash::Hasher>
    void hash(H& state) const noexcept {
        hash::hash_into(this->self().index, state);
    }
};

} // namespace rstd
