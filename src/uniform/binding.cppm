export module vrento:uniform.binding;
export import :uniform.source;
export import :uniform.buffer;
import :resource;
import rstd;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace vrento
{
struct BoundUniformOutput {
    UniformOutputId output;
    usize           slot_index {};
};

struct BoundUniformSource {
    UniformSourceOwner      owner;
    ref<dyn<UniformSource>> source;
    i32                     priority {};
    Vec<BoundUniformOutput> outputs;

    auto as_ref() const -> ref<dyn<UniformSource>> { return source; }
};

auto PrepareUniformSource(const UniformBufferLayout&, UniformSourceOwner, i32 priority,
                          ShaderMatrixConvention, ShaderMatrixAbi)
    -> Result<Option<BoundUniformSource>, UniformBufferUpdateError>;

struct UniformParameter {
    ref<str>         name;
    UniformValueView value;
};

struct UniformBindingPlan {
    UniformBufferLayout     layout;
    Vec<BoundUniformSource> sources;
    ShaderMatrixConvention  convention;
    ShaderMatrixAbi         abi;
};

class UniformBinding {
public:
    UniformBinding(resource::BufferUseHandle, Arc<UniformBindingPlan>);
    UniformBinding(resource::BufferUseHandle, UniformBufferLayout, Vec<BoundUniformSource>,
                   ShaderMatrixConvention, ShaderMatrixAbi);
    auto Buffer() const -> resource::BufferUseHandle { return m_buffer; }
    auto SetParameters(slice<UniformParameter>) -> Result<empty, UniformBufferUpdateError>;
    auto Update(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<resource::BufferContentWriter>>)
        -> Result<empty, UniformBufferUpdateError>;

private:
    struct SourceState {
        Option<Box<dyn<UniformBindingLease>>> lease;
        Option<u64>                           version;
    };
    resource::BufferUseHandle m_buffer;
    Arc<UniformBindingPlan>   m_plan;
    Vec<SourceState>          m_sources;
    Vec<u8>                   m_base;
    bool                      m_dirty { true };
};
} // namespace vrento
