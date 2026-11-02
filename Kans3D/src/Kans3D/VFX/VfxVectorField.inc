// Included by VfxAsset.cpp to keep vector field serialization with the VFX formats.
bool IsVectorFieldOperation(const std::string& op)
{
    return op == "SampleVectorField" || op == "VectorFieldBounds" || op == "VectorFieldResolution";
}
void ValidateVectorField(const InterfaceBinding& b)
{
    Check(b.Kind == "VectorField" && b.Geometry && b.Geometry->Values.size() >= 3, "VectorField requires a baked grid");
    const auto& d     = b.Geometry->Values;
    uint64_t    count = 1;
    for (int a = 0; a < 3; ++a)
    {
        Check(std::isfinite(d[0][a]) && std::isfinite(d[1][a]) && d[1][a] > d[0][a] && std::isfinite(d[1][a] - d[0][a]),
              "VectorField bounds require finite positive extents");
        Check(std::isfinite(d[2][a]) && d[2][a] >= 2 && d[2][a] <= 128 && std::floor(d[2][a]) == d[2][a],
              "VectorField resolution must be 2-128");
        count *= uint32_t(d[2][a]);
    }
    Check(d.size() == count + 3, "VectorField grid value count mismatch");
    Check(std::isfinite(b.Intensity), "Non-finite VectorField intensity");
}
YAML::Node EncodeVectorField(const VectorFieldSource& s)
{
    YAML::Node n;
    n["Bounds"]["Min"] = Vec(glm::vec4(s.Min, 0), 3);
    n["Bounds"]["Max"] = Vec(glm::vec4(s.Max, 0), 3);
    n["Source"]        = s.Source;
    if (s.Source == "File")
        n["File"] = s.File;
    else
    {
        for (int a = 0; a < 3; ++a)
            n["Bake"]["Resolution"].push_back(s.Resolution[a]);
        n["Bake"]["Frequency"] = s.Frequency;
        n["Bake"]["Octaves"]   = s.Octaves;
        n["Bake"]["Seed"]      = s.Seed;
        n["Bake"]["Strength"]  = s.Strength;
    }
    return n;
}
VectorFieldSource DecodeVectorField(const YAML::Node& n)
{
    VectorFieldSource s;
    s.Min = glm::vec3(ReadVec(n["Bounds"]["Min"], 3));
    s.Max = glm::vec3(ReadVec(n["Bounds"]["Max"], 3));
    for (int a = 0; a < 3; ++a)
        Check(s.Max[a] > s.Min[a] && std::isfinite(s.Max[a] - s.Min[a]),
              "VectorField bounds require finite positive extents");
    s.Source = n["Source"].as<std::string>();
    Check(s.Source == "File" || s.Source == "Bake", "VectorField Source must be File or Bake");
    if (s.Source == "File")
    {
        s.File    = n["File"].as<std::string>();
        auto path = std::filesystem::path(s.File);
        Check(!path.empty() && !path.is_absolute() && path == path.filename() && path.extension() == ".fga",
              "VectorField File must name an adjacent .fga file");
    }
    else
    {
        auto bake       = n["Bake"];
        auto resolution = bake["Resolution"];
        Check(resolution.IsSequence() && resolution.size() == 3, "VectorField Resolution requires three axes");
        for (int a = 0; a < 3; ++a)
        {
            s.Resolution[a] = resolution[a].as<uint32_t>();
            Check(s.Resolution[a] >= 2 && s.Resolution[a] <= 128, "VectorField resolution must be 2-128");
        }
        s.Frequency = bake["Frequency"].as<float>();
        s.Strength  = bake["Strength"].as<float>();
        s.Octaves   = bake["Octaves"].as<uint32_t>();
        s.Seed      = bake["Seed"].as<uint32_t>();
        Check(std::isfinite(s.Frequency) && std::isfinite(s.Strength), "Non-finite VectorField bake setting");
        Check(s.Octaves >= 1 && s.Octaves <= 4, "VectorField Octaves must be 1-4");
    }
    return s;
}
InterfaceBinding ImportVectorField(const VectorFieldSource& source, const std::filesystem::path& file)
{
    try
    {
        // Validate programmatically supplied settings through the canonical source parser too.
        auto                   s = DecodeVectorField(EncodeVectorField(source));
        std::vector<glm::vec4> d;
        if (s.Source == "File")
        {
            auto          path = file.parent_path() / s.File;
            std::ifstream input(path);
            Check(bool(input), "Cannot open " + path.string());
            // FGA allows whitespace and a final comma, but no missing or surplus numeric values.
            auto number = [&]() {
                std::string token;
                Check(bool(std::getline(input, token, ',')), "Short FGA value list: " + path.string());
                size_t used = 0;
                float  value;
                try
                {
                    value = std::stof(token, &used);
                }
                catch (const std::exception&)
                {
                    throw std::invalid_argument("Invalid FGA value: " + path.string());
                }
                Check(token.find_first_not_of(" \t\r\n", used) == std::string::npos && std::isfinite(value),
                      "Non-finite or invalid FGA value: " + path.string());
                return value;
            };
            glm::vec3 res;
            for (int a = 0; a < 3; ++a)
            {
                res[a] = number();
                Check(res[a] >= 2 && res[a] <= 128 && std::floor(res[a]) == res[a],
                      "FGA resolution must be 2-128: " + path.string());
            }
            glm::vec3 min, max;
            for (int a = 0; a < 3; ++a)
                min[a] = number();
            for (int a = 0; a < 3; ++a)
                max[a] = number();
            for (int a = 0; a < 3; ++a)
                Check(max[a] > min[a] && std::isfinite(max[a] - min[a]),
                      "FGA bounds require finite positive extents: " + path.string());
            // FGA bounds describe its source domain; canonical Bounds place that grid in field-local space.
            d              = {glm::vec4(s.Min, 0), glm::vec4(s.Max, 0), glm::vec4(res, 0)};
            uint32_t count = uint32_t(res.x) * uint32_t(res.y) * uint32_t(res.z);
            d.reserve(count + 3);
            for (uint32_t i = 0; i < count; ++i)
            {
                glm::vec4 value(0);
                for (int a = 0; a < 3; ++a)
                    value[a] = number();
                d.push_back(value);
            }
            std::string tail((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            Check(tail.find_first_not_of(" \t\r\n") == std::string::npos, "Long FGA value list: " + path.string());
        }
        else
        {
            d = {glm::vec4(s.Min, 0), glm::vec4(s.Max, 0), glm::vec4(glm::vec3(s.Resolution), 0)};
            d.reserve(size_t(s.Resolution.x) * s.Resolution.y * s.Resolution.z + 3);
            auto offset = CurlSeedOffset(s.Seed);
            for (uint32_t z = 0; z < s.Resolution.z; ++z)
                for (uint32_t y = 0; y < s.Resolution.y; ++y)
                    for (uint32_t x = 0; x < s.Resolution.x; ++x)
                    {
                        auto p = s.Min + (s.Max - s.Min) * glm::vec3(x, y, z) / glm::vec3(s.Resolution - 1u);
                        auto v = CurlNoise(p, s.Frequency, offset, int(s.Octaves)) * s.Strength;
                        Check(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z),
                              "Non-finite baked vector");
                        d.emplace_back(v, 0);
                    }
        }
        InterfaceBinding b;
        b.Kind     = "VectorField";
        b.Geometry = InternMeshGeometry(std::move(d));
        ValidateVectorField(b);
        return b;
    }
    catch (const std::exception& e)
    {
        throw std::invalid_argument(file.string() + ": " + e.what());
    }
}
std::vector<std::byte> EncodeVectorFieldProduct(const InterfaceBinding& b)
{
    ValidateVectorField(b);
    // Little-endian 32-bit words: magic, version, nx/ny/nz, min/max floats, then vec4 cells.
    std::vector<std::byte> bytes;
    auto                   word = [&](uint32_t v) {
        for (int a = 0; a < 4; ++a)
            bytes.push_back(std::byte((v >> (8 * a)) & 255));
    };
    auto scalar = [&](float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        word(bits);
    };
    const auto& d = b.Geometry->Values;
    word(0x4646584b);
    word(1);
    for (int a = 0; a < 3; ++a)
        word(uint32_t(d[2][a]));
    for (int i = 0; i < 2; ++i)
        for (int a = 0; a < 3; ++a)
            scalar(d[i][a]);
    for (size_t i = 3; i < d.size(); ++i)
        for (int a = 0; a < 4; ++a)
            scalar(d[i][a]);
    return bytes;
}
InterfaceBinding DecodeVectorFieldProduct(const std::byte* data, size_t size)
{
    Check(data && size >= 44, "Short VectorField product");
    size_t offset = 0;
    auto   word   = [&]() {
        uint32_t v = 0;
        for (int a = 0; a < 4; ++a)
            v |= uint32_t(data[offset++]) << (8 * a);
        return v;
    };
    auto scalar = [&]() {
        uint32_t bits = word();
        float    v;
        std::memcpy(&v, &bits, 4);
        return v;
    };
    Check(word() == 0x4646584b && word() == 1, "Invalid VectorField product header");
    glm::uvec3 res;
    uint64_t   count = 1;
    for (int a = 0; a < 3; ++a)
    {
        res[a] = word();
        Check(res[a] >= 2 && res[a] <= 128, "VectorField resolution must be 2-128");
        count *= res[a];
    }
    Check(size == 44 + count * 16, "VectorField product size mismatch");
    std::vector<glm::vec4> d(3, glm::vec4(0));
    d[2] = glm::vec4(glm::vec3(res), 0);
    for (int i = 0; i < 2; ++i)
        for (int a = 0; a < 3; ++a)
            d[i][a] = scalar();
    d.resize(count + 3);
    for (size_t i = 3; i < d.size(); ++i)
        for (int a = 0; a < 4; ++a)
        {
            d[i][a] = scalar();
            Check(std::isfinite(d[i][a]), "Non-finite VectorField product value");
        }
    InterfaceBinding b;
    b.Kind     = "VectorField";
    b.Geometry = InternMeshGeometry(std::move(d));
    ValidateVectorField(b);
    return b;
}
glm::vec3 SampleVectorField(const InterfaceBinding& b, glm::vec3 p)
{
    const auto& d = b.Geometry->Values;
    glm::ivec3  res(d[2]);
    auto        uv = (p - glm::vec3(d[0])) / (glm::vec3(d[1]) - glm::vec3(d[0]));
    for (int a = 0; a < 3; ++a)
        // Corner-grid convention. Outside a clamp axis, hold its border value.
        uv[a] = b.Tiling[a] ? uv[a] - std::floor(uv[a]) : std::clamp(uv[a], 0.f, 1.f);
    auto      grid = uv * glm::vec3(res - 1);
    auto      lo   = glm::ivec3(glm::floor(grid));
    auto      hi   = glm::min(lo + 1, res - 1);
    auto      f    = glm::fract(grid);
    glm::vec3 value(0);
    for (int z = 0; z < 2; ++z)
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
            {
                auto cell = glm::ivec3(x ? hi.x : lo.x, y ? hi.y : lo.y, z ? hi.z : lo.z);
                value += glm::vec3(d[3 + cell.x + res.x * (cell.y + res.y * cell.z)]) * (x ? f.x : 1 - f.x) *
                         (y ? f.y : 1 - f.y) * (z ? f.z : 1 - f.z);
            }
    return value * b.Intensity;
}
