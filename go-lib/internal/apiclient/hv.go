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

package apiclient

import (
	"fmt"
	"os"
	"time"

	"github.com/ProtonMail/proton-bridge/v3/pkg/algo"
)

func getProtectedHostname() string {
	hostname, err := os.Hostname()
	if err != nil {
		return "Unknown"
	}
	return algo.HashBase64SHA256(hostname)
}

func getTimeZone() string {
	zone, offset := time.Now().Zone()
	return fmt.Sprintf("%s%+d", zone, offset/3600)
}