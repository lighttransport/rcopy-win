-- rsocket API(from librdmacm)
newoption {
   trigger     = "rsocket-path",
   description = "PATH to InfiniBand rsocket library(Requires IB facility).",
   value       = "PATH"
}

main_sources = {
   "rcopy.c",
   "my_getopt.c",
   }

newaction {
   trigger     = "install",
   description = "Install the software",
   execute = function ()
      -- copy files, etc. here
   end
}

solution "RCopySolution"
   configurations { "Release", "Debug" }

   if (os.is("windows")) then
      platforms { "x64" }
   else
      platforms { "native", "x64", "x32" }
   end

   -- A project defines one build target
   project "Rcopy"
      kind "ConsoleApp"
      language "C++"
      files { main_sources, src_sources }

      includedirs {
         "./include",
      }

      defines { 'ENABLE_CJSON_GLOBAL' }

      -- MacOSX. Guess we use gcc.
      configuration { "macosx", "gmake" }

         linkoptions { "-pthread" }

      -- Windows specific
      configuration { "windows" }

         defines { 'NOMINMAX', '_LARGEFILE_SOURCE', '_FILE_OFFSET_BITS=64' }
         defines { '__STDC_CONSTANT_MACROS', '__STDC_LIMIT_MACROS' } -- c99

         if _OPTIONS['rsocket-path'] then
            defines { "ENABLE_RSOCKET" }
            defines { "_WIN64", "_X64", "_AMD64_" }
            includedirs { _OPTIONS['rsocket-path'] .. "\\Inc" }
            includedirs { _OPTIONS['rsocket-path'] .. "\\Inc\\linux" }
            libdirs { _OPTIONS['rsocket-path'] .. "\\lib" }
            links { "librdmacm", "winverbs" }
         end

         links {"ws2_32"}

      -- Linux specific
      configuration {"linux", "gmake"}
         defines { '__STDC_CONSTANT_MACROS', '__STDC_LIMIT_MACROS' } -- c99

         linkoptions { "-pthread" }

         if _OPTIONS['rsocket-path'] then
            defines { 'ENABLE_RSOCKET' }
            includedirs { "/usr/local/include" }
            libdirs { "/usr/local/lib" }
            links { "rdmacm" }
         end


      configuration "Debug"
         defines { "DEBUG" } -- -DDEBUG
         flags { "Symbols" }
         targetdir "bin"
         targetname "rcopy_debug"

      configuration "Release"
         flags { "Symbols", "Optimize" }
         targetdir "bin"

         targetname "rcopy"
