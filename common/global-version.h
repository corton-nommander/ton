/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once

namespace ton {

// See doc/GlobalVersions.md
// v15 introduces the capability-gated, run-only native transfer batch. The
// default genesis remains v14; merely supporting this version does not enable
// the wire format without capNativeTransferRuns in the live configuration.
constexpr int SUPPORTED_VERSION = 15;

}  // namespace ton
