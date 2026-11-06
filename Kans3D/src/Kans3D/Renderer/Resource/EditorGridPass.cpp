#include "kspch.h"
#include "EditorGridPass.h"
#include "Kans3D/Renderer/Renderer.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"

namespace Kans
{
    namespace
    {
        constexpr const char* Vertex   = R"glsl(#version 450
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2 - 1, 0, 1);
}
)glsl";
        constexpr const char* Fragment = R"glsl(#version 450
layout(set=0,binding=0,std140) uniform GridParameters {
    mat4 InverseVP; mat4 VP;
    vec4 ViewportDepth; // width, height, near NDC, NDC-to-window depth scale
    vec4 Origin; vec4 U; vec4 V; vec4 Normal; vec4 Eye;
    vec4 Settings; // cell size, major interval, pixel width, fade radius
    vec4 Minor; vec4 Major; vec4 AxisU; vec4 AxisV; vec4 DepthPlane;
} p;
layout(location=0) out vec4 Color;
vec3 unproject(vec2 xy, float z) {
    vec4 h = p.InverseVP * vec4(xy,z,1);
    return h.xyz / h.w;
}
vec4 over(vec4 below, vec4 above) {
    // Premultiplied internally; the framebuffer blend uses straight alpha.
    return above + below * (1-above.a);
}
float lines(vec2 q, vec2 gradient, float cell) {
    vec2 pixels = abs(fract(q/cell + .5)-.5) * cell / gradient;
    vec2 coverage = clamp(vec2(p.Settings.z*.5+.5)-pixels,0,1);
    // Suppress each direction before it becomes subpixel, including grazing views.
    coverage *= smoothstep(vec2(2),vec2(6),cell/fwidth(q));
    return max(coverage.x,coverage.y);
}
vec4 layer(vec2 q, vec2 gradient, float cell) {
    float minor = lines(q,gradient,cell) * p.Minor.a;
    float major = lines(q,gradient,cell*p.Settings.y) * p.Major.a;
    // Major replaces the minor at their shared intersections.
    return over(vec4(p.Minor.rgb*minor,minor), vec4(p.Major.rgb*major,major));
}
void main() {
    vec2 ndc = gl_FragCoord.xy/p.ViewportDepth.xy*2-1;
    vec3 start = unproject(ndc,p.ViewportDepth.z);
    vec3 end = unproject(ndc,1);
    vec3 ray = end-start;
    float denominator = dot(ray,p.Normal.xyz);
    float t = dot(p.Origin.xyz-start,p.Normal.xyz) /
              (abs(denominator)>1e-8 ? denominator : 1e-8);
    vec3 hit = start + ray*t;
    vec3 local = hit-p.Origin.xyz;
    vec2 q = vec2(dot(local,p.U.xyz),dot(local,p.V.xyz));
    vec2 gradient = max(vec2(length(vec2(dFdx(q.x),dFdy(q.x))),
                             length(vec2(dFdx(q.y),dFdy(q.y)))),vec2(1e-7));
    float footprint = max(fwidth(q).x,fwidth(q).y);
    // Twelve pixels per minor cell; a decade of zoom blends the entire pair of
    // minor/major layers into the next pair. No clamp at the one-metre level.
    float lod = clamp(log(max(footprint*12/p.Settings.x,1e-8))/log(10.0),-6,8);
    float cell = p.Settings.x*pow(10.0,floor(lod));
    vec4 grid = mix(layer(q,gradient,cell),layer(q,gradient,cell*10),
                    smoothstep(0,1,fract(lod)));
    float width = p.Settings.z*.75+.5;
    // U-axis is v=0; V-axis is u=0. Axis colour wins over grey lines.
    float u = clamp(width-abs(q.y)/gradient.y,0,1)*p.AxisU.a;
    float v = clamp(width-abs(q.x)/gradient.x,0,1)*p.AxisV.a;
    grid = over(over(grid,vec4(p.AxisU.rgb*u,u)),vec4(p.AxisV.rgb*v,v));
    vec3 fromEye = hit-p.Eye.xyz;
    float radius = length(vec2(dot(fromEye,p.U.xyz),dot(fromEye,p.V.xyz)));
    float fade = 1-smoothstep(p.Settings.w*.6,p.Settings.w,radius);
    fade *= smoothstep(.015,.08,abs(denominator)/max(length(ray),1e-8));
    fade *= 1-smoothstep(.8,1,t); // Also dissolve before a short camera far plane.
    vec4 clip = p.VP*vec4(hit,1);
    // A projected plane has affine window depth. Evaluate it directly instead
    // of accumulating inverse-VP/intersection/reprojection rounding at grazing angles.
    float windowDepth = dot(p.DepthPlane.xyz,vec3(ndc,1));
    // Derivatives above must be evaluated before divergent ray rejection.
    if (t<0 || t>1 || abs(denominator)<1e-8 || clip.w<=0 ||
        windowDepth<0 || windowDepth>1 || grid.a*fade<1e-5) discard;
    // Pull only the test depth 0.002% toward the near plane along this pixel's
    // ray. This is a relative linear-view-depth tolerance for both camera types;
    // the procedural position/coverage stays on the exact selected plane.
    float nearW = (p.VP*vec4(start,1)).w;
    float weight = 2e-5*nearW/mix(clip.w,nearW,2e-5);
    // Four D24 quanta bound rasterization/float interpolation roundoff. Use the
    // larger of that precision floor and the relative view-depth adjustment.
    gl_FragDepth = max(0,windowDepth-max(windowDepth*weight,4.0/16777215.0));
    Color = vec4(grid.rgb/max(grid.a,1e-8),grid.a*fade);
}
)glsl";
        struct Parameters
        {
            glm::mat4 InverseVP, VP;
            glm::vec4 ViewportDepth, Origin, U, V, Normal, Eye, Settings, Minor, Major, AxisU, AxisV, DepthPlane;
        };
        float Positive(float value, float fallback) { return std::isfinite(value) && value > 0 ? value : fallback; }
    } // namespace

    void EditorGridPass::Record(Renderer&                            renderer,
                                RenderHardware::CommandList&         commands,
                                const RenderView&                    view,
                                const ViewGrid&                      grid,
                                const RenderHardware::RenderingDesc& target)
    {
        using namespace RenderHardware;
        if (!view.Grid.Enabled || !view.Viewport.x || !view.Viewport.y)
            return;
        auto device = renderer.GetDevice();
        auto desc   = renderer.ScenePipeline("Infinite editor grid", Vertex, Fragment, false);
        desc.ColorFormats.clear();
        desc.Blends.clear();
        for (size_t i = 0; i < target.Colors.size(); ++i)
        {
            desc.ColorFormats.push_back(target.Colors[i].Target->GetDesc().PixelFormat);
            BlendState blend;
            blend.Enabled          = true;
            blend.SourceColor      = BlendFactor::SourceAlpha;
            blend.DestinationColor = BlendFactor::OneMinusSourceAlpha;
            blend.WriteMask        = i == 0 ? 7 : 0;
            desc.Blends.push_back(blend);
        }
        desc.DepthStencilFormat        = target.Depth.Target->GetDesc().PixelFormat;
        desc.DepthStencil.DepthWrite   = false;
        desc.DepthStencil.DepthCompare = CompareOp::LessEqual;
        const auto  coordinates        = device->GetCoordinateConventions();
        Parameters  data {};
        const auto& projection = view.Scene.sceneCamera.camera.GetProjectionMatrix();
        const auto& viewMatrix = view.Scene.sceneCamera.viewMatrix;
        data.InverseVP         = glm::inverse(DeviceProjection(projection, coordinates) * viewMatrix);
        // Match the material pass's floating-point association for depth projection.
        data.VP            = DeviceProjection(projection * viewMatrix, coordinates);
        data.ViewportDepth = {view.Viewport.x,
                              view.Viewport.y,
                              coordinates.Depth == DepthRange::ZeroToOne ? 0.0f : -1.0f,
                              coordinates.Depth == DepthRange::ZeroToOne ? 1.0f : .5f};
        data.Origin        = glm::vec4(grid.Origin, 0);
        data.U             = grid.Surface == ViewGrid::Plane::YZ ? glm::vec4(0, 1, 0, 0) : glm::vec4(1, 0, 0, 0);
        data.V             = grid.Surface == ViewGrid::Plane::XY ? glm::vec4(0, 1, 0, 0) : glm::vec4(0, 0, 1, 0);
        data.Normal        = glm::vec4(glm::cross(glm::vec3(data.U), glm::vec3(data.V)), 0);
        // Transform the world plane into clip space in double precision. Its
        // equation yields window Z = ax*NDC.x + ay*NDC.y + constant for either projection.
        const glm::dvec4 worldPlane(glm::dvec3(data.Normal),
                                    -glm::dot(glm::dvec3(data.Normal), glm::dvec3(data.Origin)));
        const auto       clipPlane = glm::transpose(glm::inverse(glm::dmat4(data.VP))) * worldPlane;
        if (std::abs(clipPlane.z) < 1e-12)
            return; // A plane through the eye projects to a line, with no surface coverage.
        const auto coefficients = -glm::dvec3(clipPlane.x, clipPlane.y, clipPlane.w) / clipPlane.z;
        data.DepthPlane         = {coefficients.x * data.ViewportDepth.w,
                                   coefficients.y * data.ViewportDepth.w,
                                   coefficients.z * data.ViewportDepth.w + (1 - data.ViewportDepth.w),
                                   0};
        data.Eye                = glm::inverse(view.Scene.sceneCamera.viewMatrix)[3];
        const float cell        = Positive(view.Grid.CellSize, 1);
        const float height      = std::abs(glm::dot(glm::vec3(data.Eye - data.Origin), glm::vec3(data.Normal)));
        data.Settings           = {cell,
                                   std::max(1u, view.Grid.MajorEvery),
                                   Positive(view.Grid.LineWidth, 1),
                                   Positive(view.Grid.FadeDistance, 100) * std::max(1.0f, height / (10 * cell))};
        data.Minor              = view.Grid.MinorColor;
        data.Major              = view.Grid.MajorColor;
        data.AxisU              = grid.Surface == ViewGrid::Plane::YZ ? view.Grid.YAxisColor : view.Grid.XAxisColor;
        data.AxisV              = grid.Surface == ViewGrid::Plane::XY ? view.Grid.YAxisColor : view.Grid.ZAxisColor;
        auto buffer             = device->CreateBuffer({sizeof(data), BufferUsage::Uniform, MemoryUsage::Upload});
        buffer->Write(0, &data, sizeof(data));
        BindGroupDesc group;
        group.Layout = desc.GroupLayouts[0];
        group.Entries.push_back({0, buffer, 0, sizeof(data)});
        commands.BeginRendering(target);
        commands.SetViewport({0, 0, float(view.Viewport.x), float(view.Viewport.y)});
        commands.SetScissor({0, 0, view.Viewport.x, view.Viewport.y});
        commands.BindPipeline(renderer.CachedPipeline(desc));
        commands.BindGroup(device->CreateBindGroup(group));
        commands.Draw(3);
        commands.EndRendering();
    }
} // namespace Kans
