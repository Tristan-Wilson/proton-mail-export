// Copyright (c) 2023 Proton AG
//
// This file is part of Proton Export Tool.
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
// along with Proton Export Tool.  If not, see <https://www.gnu.org/licenses/>.

#include "tasks/backup_task.hpp"
#include <etsession.hpp>
#include <iostream>

BackupTask::BackupTask(etcpp::Session& session, const std::filesystem::path& backupPath) :
    mBackup(session.newBackup(backupPath.u8string().c_str())) {}

void BackupTask::onProgress(float progress) {
    updateProgress(progress);
}

void BackupTask::run() {
    mBackup.start(*this);
}

void BackupTask::cancel() {
    mBackup.cancel();
}

std::string_view BackupTask::description() const {
    return "Export Mail";
}
