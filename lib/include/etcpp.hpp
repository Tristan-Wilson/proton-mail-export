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

#include <string>

struct etSession;

namespace etcpp {
class Exception final : public std::exception {
   private:
    friend class Session;
    std::string mWhat;

   public:
    Exception(std::string_view what);
    const char* what() const noexcept;
};

class Session final {
   private:
    etSession* mPtr;

   public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session(Session&&) noexcept;
    Session& operator=(const Session&) = delete;
    Session& operator=(Session&& rhs) noexcept;

    [[nodiscard]] std::string hello() const;
    void helloError() const;

   private:
    template <class F>
    void wrapCCall(F func);

    template <class F>
    void wrapCCall(F func) const;

    template <class F, class OUT>
    void wrapCCallOut(OUT& out, F func);

    template <class F, class OUT>
    void wrapCCallOut(OUT& out, F func) const;
};
}    // namespace etcpp