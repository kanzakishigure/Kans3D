workspace "Hazel"
    architecture"x64"
    startproject "Sandbox"
configurations 
{ 
    "Debug", 
    "Release",
    "Dist" 
}
outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
--include 文件夹 采用相对路径
IncludeDir ={}
IncludeDir["GLFW"] = "Hazel/vendor/GLFW/include"
IncludeDir["Glad"] = "Hazel/vendor/Glad/include"
IncludeDir["ImGui"] = "Hazel/vendor/imgui"
IncludeDir["GLM"] = "Hazel/vendor/glm"
IncludeDir["stb_image"] = "Hazel/vendor/stb_image"
IncludeDir["entt"] = "Hazel/vendor/entt/include"
IncludeDir["assimp"] = "Hazel/vendor/assimp/include"

Library = {}
Library["Assimp_Debug"] = "%{_WORKING_DIR}/Hazel/vendor/assimp/bin/Debug/assimp-vc142-mtd.lib"
Library["Assimp_Release"] = "%{_WORKING_DIR}/Hazel/vendor/assimp/bin/Release/assimp-vc142-mt.lib"

Binaries = {}
Binaries["Assimp_Debug"] = "%{_WORKING_DIR}/Hazel/vendor/assimp/bin/Debug/assimp-vc142-mtd.dll"
Binaries["Assimp_Release"] = "%{_WORKING_DIR}/Hazel/vendor/assimp/bin/Release/assimp-vc142-mt.dll"

include "Hazel/vendor/GLFW"
include "Hazel/vendor/Glad"
include "Hazel/vendor/imgui"


--相当于将Hazel/vendor/GLFW下的remake5文件直接复制粘贴到此处
project "Hazel"    
    location"Hazel"
    kind "StaticLib"
    language "C++"
    cppdialect"c++17"
    staticruntime"on"


    targetdir ("bin/" ..outputdir.. "/%{prj.name}")
    objdir ("bin-int/" ..outputdir.. "/%{prj.name}")
    pchheader "hzpch.h"
    pchsource "Hazel/src/hzpch.cpp"
    files 
    { 
        "%{prj.name}/src/**.h", 
        "%{prj.name}/src/**.cpp" ,
        "%{prj.name}/vendor/stb_image/**.h", 
        "%{prj.name}/vendor/stb_image/**.cpp" ,
        "%{prj.name}/vendor/glm/glm/**.hpp",
        "%{prj.name}/vendor/glm/glm/**.inl"
    }
    includedirs 
    { 
        "%{prj.name}/vendor/spdlog/include",
        "%{prj.name}/src",
        "%{prj.name}/src/Hazel",
        "%{IncludeDir.GLFW}",
        "%{IncludeDir.Glad}",
        "%{IncludeDir.ImGui}",
        "%{IncludeDir.GLM}",
        "%{IncludeDir.stb_image}",
        "%{IncludeDir.entt}",
        "%{IncludeDir.assimp}"
        
    }
    links
    {
        "GLFW",
        "Glad",
        "ImGui",
        "opengl32.lib"
        
    }
    filter "system:windows" 
        systemversion "latest" 

        defines
        {
            "HZ_PLATFORM_WINDOWS",
            "HZ_BUILD_DLL",
            "GLFW_INCLUDE_NONE",
            "_CRT_SECURE_NO_WARNINGS"
        }



    filter "configurations:Debug"
        defines "HZ_DEBUG" 
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        defines  "HZ_RELEASE" 
        runtime "Release"
        optimize "on"
    filter "configurations:Dist"
        defines  "HZ_DIST" 
        runtime "Release"
        optimize "on"
        

project "Sandbox"
    kind "ConsoleApp"
    location "Sandbox"
    language "C++"
    cppdialect "c++17"     
    staticruntime "on"
         
    targetdir ("bin/" ..outputdir.. "/%{prj.name}")
    objdir ("bin-int/" ..outputdir.. "/%{prj.name}")

    files
    { 
        "%{prj.name}/src/**.h", 
        "%{prj.name}/src/**.cpp" 
    }
    includedirs 
    { 
        "Hazel/vendor/spdlog/include",
        "Hazel/src",
        "%{IncludeDir.GLM}",
        "%{IncludeDir.ImGui}",
        "%{IncludeDir.entt}",
        "%{IncludeDir.assimp}"
    }
    links
    {
        "Hazel"
    }
    filter "system:windows"
        systemversion "latest" 

        defines
        {
            "HZ_PLATFORM_WINDOWS"      
        }



    filter "configurations:Debug"
        defines "HZ_DEBUG" 
        runtime "Debug"
        symbols "on"
    filter "configurations:Release"
        defines  "HZ_RELEASE" 
        runtime "Release"
        optimize "on"
    filter "configurations:Dist"
        defines  "HZ_DIST" 
        runtime "Release"
        optimize "on"


project "KansEditor"
        location"KansEditor"  
        kind "ConsoleApp"
        language "C++"
        cppdialect "c++17"     
        staticruntime "on"
             
        targetdir ("bin/" ..outputdir.. "/%{prj.name}")
        objdir ("bin-int/" ..outputdir.. "/%{prj.name}")
    
        files
        { 
            "%{prj.name}/src/**.h", 
            "%{prj.name}/src/**.cpp" 
        }
        includedirs 
        { 
            "Hazel/vendor/assimp/include",
            "Hazel/vendor/spdlog/include",
            "Hazel/src",
            "%{IncludeDir.GLM}",
            "%{IncludeDir.ImGui}",
            "%{IncludeDir.entt}",
            "%{IncludeDir.assimp}"

        }
        links
        {
            "Hazel"
        }
        filter "system:windows"
            systemversion "latest" 
    
            defines
            {
                "HZ_PLATFORM_WINDOWS"      
            }
    
    
    
        filter "configurations:Debug"
            defines "HZ_DEBUG" 
            runtime "Debug"
            symbols "on"
            links
            {
                "%{Library.Assimp_Debug}"
            }
            postbuildcommands 
            {
                '{COPY} "%{Binaries.Assimp_Debug}" "%{cfg.targetdir}"'
            }
           
        filter "configurations:Release"
            defines  "HZ_RELEASE" 
            runtime "Release"
            optimize "on"
            links
            {
                "%{Library.Assimp_Release}"
            }
            postbuildcommands 
            {
                '{COPY} "%{Binaries.Assimp_Release}" "%{cfg.targetdir}"'
            }
            
        filter "configurations:Dist"
            defines  "HZ_DIST" 
            runtime "Release"
            optimize "on"
            links
            {
                "%{Library.Assimp_Release}"
            }
            postbuildcommands 
            {
                '{COPY} "%{Binaries.Assimp_Release}" "%{cfg.targetdir}"'
            }