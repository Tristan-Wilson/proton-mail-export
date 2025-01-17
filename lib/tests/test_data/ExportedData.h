// Copyright (c) 2024 Proton AG
//
// This file is part of Proton Mail Bridge.
//
// Proton Mail Bridge is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Proton Mail Bridge is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Proton Mail Bridge. If not, see <https://www.gnu.org/licenses/>.

#ifndef EXPORTED_DATA_H
#define EXPORTED_DATA_H

#include <filesystem>

void createTestBackup(std::filesystem::path const& dir); ///< Create a test backup in the specified folder
void addSkippedAndFailingMessages(std::filesystem::path const &dir); /// Add a message that will be skipped and a message that will fail to an existing backup.

#endif //EXPORTED_DATA_H