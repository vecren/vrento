export module vrento:resource_registry.physical_bindings;
import rstd;

export namespace vrento::resource_registry
{
struct PhysicalBindingKey {
    bool           buffer {};
    rstd::uint64_t resource {};
    friend bool    operator==(PhysicalBindingKey, PhysicalBindingKey) = default;
};
class PhysicalBindingLayout {
    using Keys = rstd::vec::Vec<PhysicalBindingKey>;
    rstd::sync::Arc<Keys> keys_;
    explicit PhysicalBindingLayout(rstd::sync::Arc<Keys> keys): keys_(rstd::move(keys)) {}
    friend class PreparedResourceTable;

public:
    auto keys() const [[clang::lifetimebound]] -> rstd::slice<PhysicalBindingKey> {
        return keys_->as_slice();
    }
    auto clone() const -> PhysicalBindingLayout { return PhysicalBindingLayout(keys_.clone()); }
};
} // namespace vrento::resource_registry
