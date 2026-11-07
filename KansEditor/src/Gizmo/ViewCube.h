#pragma once
#include <array>
#include <optional>
#include <utility>
#include <vector>
#include <glm/glm.hpp>
#include "KansEditorCore/EditorCamera.h"

// The view cube in the viewport corner, with no ImGui: a cube turning with the camera whose
// faces, edges and corners each look from one of 26 directions. Each face is cut into 3 x 3
// cells: the middle one is the face, the side ones its edges and the corner ones its corners.
// Clicking a cell aligns the view from that direction (again: from the opposite side) with a
// short animation; dragging the cube orbits.
namespace Kans::Gizmo
{
    // A direction on the cube: each component -1, 0 or 1, not all 0.
    using CubeDirection = glm::ivec3;

    // One cell of a face that faces the camera.
    struct ViewCubeCell
    {
        std::array<glm::vec2, 4> Screen {};
        CubeDirection            Direction {0, 0, 1};
        int                      Face   = 0;    // 0-5: +X, +Y, +Z, -X, -Y, -Z.
        float                    Depth  = 0.0f; // Larger is farther from the camera.
        float                    Facing = 0.0f; // The face normal toward the camera, 0-1.
    };
    // A face label at the face's middle.
    struct ViewCubeLabel
    {
        glm::vec2   Screen {0};
        float       Facing = 0.0f;
        int         Face   = 0;
        const char* Text   = "";
    };

    // Cells within this fraction of a face's half-width from its middle are the face itself.
    inline constexpr float ViewCubeFaceExtent = 0.6f;

    // Where a point on the cube ([-1, 1] on each axis) is drawn for a view matrix, with the cube
    // `half` pixels from its middle to a face at `centre` (a mild perspective).
    glm::vec2 ViewCubeProject(const glm::mat4& view, glm::vec2 centre, float half, glm::vec3 point);
    // The cells of the faces turned toward the camera, sorted back to front.
    std::vector<ViewCubeCell> ViewCubeCells(const glm::mat4& view, glm::vec2 centre, float half);
    // The labels of those faces, back to front.
    std::vector<ViewCubeLabel> ViewCubeLabels(const glm::mat4& view, glm::vec2 centre, float half);
    // The front-most cell under `mouse`: its direction.
    std::optional<CubeDirection> PickViewCube(const std::vector<ViewCubeCell>& cells, glm::vec2 mouse);
    const char* ViewCubeFaceName(int face); // Right, Top, Front, Left, Bottom, Back.

    // The named view looking from a face direction (+Y: Top, +Z: Front, +X: Right); empty for
    // edges and corners.
    std::optional<EditorCamera::View> FaceView(CubeDirection direction);
    // The yaw and pitch that look at the focus from `direction` (faces: EditorCamera::ViewAngles).
    std::pair<float, float> ViewAnglesFrom(CubeDirection direction);
    // Whether the camera already looks from that side (within about a degree).
    bool IsLookingFrom(const EditorCamera& camera, glm::vec3 direction);
    // Yaw and pitch between two orientations at t (0..1, eased), turning the short way in yaw.
    std::pair<float, float> InterpolateView(std::pair<float, float> from, std::pair<float, float> to, float t);

    struct ViewCubeInput
    {
        glm::vec2 Mouse {0};
        bool      Over    = false; // The pointer is over the cube's area and nothing else has it.
        bool      Pressed = false, Down = false;
        float     DeltaTime = 0.0f;
    };

    class ViewCube
    {
    public:
        static constexpr float AnimationSeconds = 0.25f;
        static constexpr float DragPixels       = 3.0f; // A press moving further orbits.

        // One frame: advances an alignment in progress, then hover, press, drag and release with
        // the cube at `centre`, `half` pixels to a face. `input.Over` false skips the pointer.
        void Update(EditorCamera& camera, glm::vec2 centre, float half, const ViewCubeInput& input);
        // Advances only the alignment (the cube is hidden, as with the scene camera).
        void Animate(EditorCamera& camera, float deltaTime);
        // Starts turning to look from `direction`; from the side already looked from, the opposite.
        void Align(const EditorCamera& camera, CubeDirection direction);

        const std::vector<ViewCubeCell>&  GetCells() const { return m_Cells; }
        const std::vector<ViewCubeLabel>& GetLabels() const { return m_Labels; }
        std::optional<CubeDirection>      GetHovered() const { return m_Hovered; }
        bool IsPressed() const { return m_Pressed; }
        bool IsOrbiting() const { return m_Orbiting; }
        bool IsAnimating() const { return m_Animation.has_value(); }
        // The view an alignment ended in on the last Update ("Top", "Perspective"...), once.
        const char* TakeFinishedViewName();

    private:
        struct Animation
        {
            std::pair<float, float>           From, To;
            float                             Time = 0.0f;
            std::optional<EditorCamera::View> Face;
            int                               FaceIndex = 0; // When Face is set.
        };
        std::vector<ViewCubeCell>    m_Cells;
        std::vector<ViewCubeLabel>   m_Labels;
        std::optional<CubeDirection> m_Hovered, m_PressedOn;
        bool                         m_Pressed = false, m_Orbiting = false;
        glm::vec2                    m_PressAt {0}, m_Last {0};
        std::optional<Animation>     m_Animation;
        const char*                  m_Finished = nullptr;
    };
} // namespace Kans::Gizmo
