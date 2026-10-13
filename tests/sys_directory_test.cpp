// SPDX-License-Identifier: GPL-3.0-or-later

#include <iostream>
#include <string>

#include "Common/FileUtil.h"

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;

    const std::string actual = File::GetSysDirectory();
    if (actual != argv[1])
    {
        std::cerr << "Expected Sys directory: " << argv[1] << '\n'
                  << "Actual Sys directory: " << actual << '\n';
        return 1;
    }
    return 0;
}
