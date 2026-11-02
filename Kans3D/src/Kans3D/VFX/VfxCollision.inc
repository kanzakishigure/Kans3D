// Collision geometry and response live here; the CPU interpreter and all outputs
// of a Collision node use the same result. Distances are exact world-space SDFs.
namespace
{
    float CollisionDistance(const CollisionShape& s, glm::vec3 p, glm::vec3& normal)
    {
        p -= glm::vec3(s.CenterRadius);
        auto kind = ColliderKind(s.Meta.x);
        if (kind == ColliderKind::Plane)
        {
            normal = glm::vec3(s.AxisY);
            return glm::dot(p, normal);
        }
        if (kind == ColliderKind::Sphere || kind == ColliderKind::Capsule)
        {
            if (kind == ColliderKind::Capsule)
                p -= glm::vec3(s.AxisY) * glm::clamp(glm::dot(p, glm::vec3(s.AxisY)), -s.AxisY.w, s.AxisY.w);
            float length = glm::length(p);
            normal       = length > 1e-8f ? p / length : glm::vec3(0, 1, 0);
            return length - s.CenterRadius.w;
        }
        glm::mat3 axes(glm::vec3(s.AxisX), glm::vec3(s.AxisY), glm::vec3(s.AxisZ));
        auto      local   = glm::transpose(axes) * p;
        auto      q       = glm::abs(local) - glm::vec3(s.AxisX.w, s.AxisY.w, s.AxisZ.w);
        auto      outside = glm::max(q, glm::vec3(0));
        float     length  = glm::length(outside);
        glm::vec3 n(0);
        if (length > 1e-8f)
            n = outside / length;
        else
        {
            int axis = q.x > q.y ? (q.x > q.z ? 0 : 2) : (q.y > q.z ? 1 : 2);
            n[axis]  = 1;
        }
        for (int axis = 0; axis < 3; ++axis)
            if (local[axis] < 0)
                n[axis] = -n[axis];
        normal = axes * n;
        return length + std::min(std::max({q.x, q.y, q.z}), 0.f);
    }
    CollisionResult
    CollideParticle(const Instruction& i, const std::vector<Value>& registers, const ExecutionContext& c)
    {
        const auto& a        = i.Args;
        auto        v        = [&](size_t n) -> const Value& { return registers.at(a.at(n)); };
        auto        previous = glm::vec3(v(0).Data), position = glm::vec3(v(1).Data), velocity = glm::vec3(v(2).Data);
        bool        local   = i.PositionSpace == Space::Local;
        auto        inverse = local ? glm::inverse(c.LocalToWorld) : glm::mat4(1);
        if (local)
        {
            previous = glm::vec3(c.LocalToWorld * glm::vec4(previous, 1));
            position = glm::vec3(c.LocalToWorld * glm::vec4(position, 1));
            velocity = glm::mat3(c.LocalToWorld) * velocity;
        }
        auto  size = glm::abs(v(3).Data), scale = glm::abs(v(4).Data);
        float mesh   = v(12).Data.x;
        float radius = v(9).Data.x >= 0 ?
                           v(9).Data.x :
                           (mesh >= 0 ? mesh * std::max({scale.x, scale.y, scale.z}) : .5f * std::max(size.x, size.y)) *
                               std::max(0.f, v(8).Data.x);
        if (local)
            radius *= std::max({glm::length(glm::vec3(c.LocalToWorld[0])),
                                glm::length(glm::vec3(c.LocalToWorld[1])),
                                glm::length(glm::vec3(c.LocalToWorld[2]))});
        float      dt = std::max(0.f, v(11).Data.x), best = 2;
        glm::vec3  normal(0), bodyVelocity(0), center(0);
        const auto mode = CollisionMode(v(5).Integer);
        if (mode == CollisionMode::SceneQueries && c.Physics)
        {
            auto  delta  = position - previous;
            float length = glm::length(delta);
            if (length > 1e-8f)
            {
                QueryFilter filter;
                filter.LayerMask = c.CollisionMask;
                filter.Triggers  = QueryTriggerInteraction::Ignore;
                Physics::WorldShape pose;
                pose.Position = previous;
                Physics::Sphere sphere;
                sphere.Radius = radius;
                auto hit      = radius > 1e-4f ? c.Physics->Cast(sphere, pose, delta / length, length, filter) :
                                                 c.Physics->Raycast(previous, delta / length, length, filter);
                if (hit)
                {
                    best   = glm::clamp(hit->Distance / length, 0.f, 1.f);
                    normal = hit->Normal;
                    center = hit->Point + normal * radius;
                    if (auto body = c.Physics->GetBodyState(hit->BodyEntity))
                        bodyVelocity =
                            body->LinearVelocity + glm::cross(body->AngularVelocity, hit->Point - body->CenterOfMass);
                }
            }
        }
        // A stationary particle has no query direction. Moving analytic bodies
        // can still sweep into it; use the shared snapshot when the query misses.
        if (mode != CollisionMode::DepthBuffer && c.Colliders && best > 1)
        {
            for (const auto& shape : c.Colliders->Shapes)
            {
                if (shape.Meta.y > 31 || !(c.CollisionMask & (1u << shape.Meta.y)))
                    continue;
                auto  motion = position - previous - glm::vec3(shape.Velocity) * dt;
                auto  start  = previous + glm::vec3(shape.Velocity) * dt;
                float speed = glm::length(motion), t = 0;
                // Conservative advancement uses the SDF's Lipschitz bound. It
                // cannot skip thin surfaces, including rounded box corners.
                for (int step = 0; step < 96 && t <= 1 && t < best; ++step)
                {
                    glm::vec3 n;
                    auto      p        = start + motion * t;
                    float     distance = CollisionDistance(shape, p, n) - radius;
                    if (distance <= 1e-5f)
                    {
                        if (distance < -1e-5f || glm::dot(motion, n) < -1e-8f)
                        {
                            best         = t;
                            normal       = n;
                            center       = p - n * distance;
                            bodyVelocity = glm::vec3(shape.Velocity) +
                                           glm::cross(glm::vec3(shape.AngularVelocity),
                                                      center - n * radius - glm::vec3(shape.CenterRadius));
                        }
                        break;
                    }
                    if (speed < 1e-8f)
                        break;
                    t += distance / speed;
                }
            }
        }
        CollisionResult result;
        result.Hit = best <= 1;
        if (result.Hit)
        {
            auto  relative = velocity - bodyVelocity;
            float vn       = glm::dot(relative, normal);
            auto  tangent  = relative - vn * normal;
            float bounce   = std::max(0.f, -vn) * glm::clamp(v(6).Data.x, 0.f, 1.f);
            if (bounce < std::max(0.f, v(10).Data.x))
                bounce = 0;
            velocity =
                bodyVelocity + tangent * (1 - glm::clamp(v(7).Data.x, 0.f, 1.f)) + normal * (vn > 0 ? vn : bounce);
            position = center + (velocity - bodyVelocity) * dt * (1 - best);
        }
        result.Contact  = result.Hit ? center - normal * radius : position;
        result.Position = position;
        result.Velocity = velocity;
        result.Normal   = normal;
        if (local)
        {
            result.Position = glm::vec3(inverse * glm::vec4(position, 1));
            result.Contact  = glm::vec3(inverse * glm::vec4(result.Contact, 1));
            result.Velocity = glm::mat3(inverse) * velocity;
            result.Normal   = result.Hit ? glm::normalize(glm::transpose(glm::mat3(c.LocalToWorld)) * normal) : normal;
        }
        return result;
    }
    Value CollisionValue(const Instruction& i, const CollisionResult& hit)
    {
        if (i.Output == "Hit")
            return Value::Bool(hit.Hit);
        return Value::Vector(glm::vec4(i.Output == "Velocity" ? hit.Velocity :
                                       i.Output == "Normal"   ? hit.Normal :
                                       i.Output == "Contact"  ? hit.Contact :
                                                                hit.Position,
                                       0),
                             i.Kind);
    }
} // namespace
