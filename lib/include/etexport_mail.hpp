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

#pragma once

#include <exception>
#include <string>

extern "C" {
struct etExportMail;
}

namespace etcpp {

class Session;

class ExportMailException final : public std::exception {
   private:
    friend class Session;
    std::string mWhat;

   public:
    explicit ExportMailException(std::string_view what);
    [[nodiscard]] const char* what() const noexcept;
};

class ExportMailCallback {
   public:
    enum class Reply {
        Continue,
        Cancel,
    };

    ExportMailCallback() = default;
    virtual ~ExportMailCallback() = default;

    virtual Reply onProgress(float progress) = 0;
};

class ExportMail final {
    friend class Session;

   private:
    const Session& mSession;
    etExportMail* mPtr;

   protected:
    ExportMail(const Session& session, etExportMail* ptr);

   public:
    ~ExportMail();
    ExportMail(const ExportMail&) = delete;
    ExportMail(ExportMail&&) noexcept = delete;
    ExportMail& operator=(const ExportMail&) = delete;
    ExportMail& operator=(ExportMail&& rhs) noexcept = delete;

    void start(ExportMailCallback& cb);

   private:
    template <class F>
    void wrapCCall(F func);

    template <class F>
    void wrapCCall(F func) const;
};
}    // namespace etcpp