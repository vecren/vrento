export module vrento.shader_types;

export namespace vrento
{

enum class ShaderType
{
    VERTEX,
    GEOMETRY,
    FRAGMENT
};
enum class ShaderScalarKind
{
    Unknown,
    Float,
    SignedInteger,
    UnsignedInteger,
    Boolean
};
enum class ShaderMatrixMajor
{
    None,
    Row,
    Column
};
enum class ShaderMatrixConvention
{
    ColumnVector,
    RowVector
};
enum class ShaderMatrixAbi
{
    NativeSpirv,
    Hlsl
};

} // namespace vrento
