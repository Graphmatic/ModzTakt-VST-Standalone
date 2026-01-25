/* =========================================================================================

   This is an auto-generated file: Any edits you make may be overwritten!

*/

#pragma once

namespace BinaryData
{
    extern const char*   checkbox_off_svg;
    const int            checkbox_off_svgSize = 1172;

    extern const char*   checkbox_on_svg;
    const int            checkbox_on_svgSize = 1589;

    extern const char*   checkbox_on_green_svg;
    const int            checkbox_on_green_svgSize = 1593;

    extern const char*   checkbox_on_orange_svg;
    const int            checkbox_on_orange_svgSize = 1595;

    extern const char*   checkbox_on_purple_svg;
    const int            checkbox_on_purple_svgSize = 1595;

    extern const char*   checkbox_on_red_svg;
    const int            checkbox_on_red_svgSize = 1589;

    extern const char*   scope_png;
    const int            scope_pngSize = 6243;

    extern const char*   TODO_md;
    const int            TODO_mdSize = 91;

    // Number of elements in the namedResourceList and originalFileNames arrays.
    const int namedResourceListSize = 8;

    // Points to the start of a list of resource names.
    extern const char* namedResourceList[];

    // Points to the start of a list of resource filenames.
    extern const char* originalFilenames[];

    // If you provide the name of one of the binary resource variables above, this function will
    // return the corresponding data and its size (or a null pointer if the name isn't found).
    const char* getNamedResource (const char* resourceNameUTF8, int& dataSizeInBytes);

    // If you provide the name of one of the binary resource variables above, this function will
    // return the corresponding original, non-mangled filename (or a null pointer if the name isn't found).
    const char* getNamedResourceOriginalFilename (const char* resourceNameUTF8);
}
