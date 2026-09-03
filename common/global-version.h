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
// v15 introduces the capability-gated, run-only native transfer batch. v16
// adds the optional fixed-depth native payment-lane policy. The default
// genesis remains v14; merely supporting either version does not enable its
// feature without the matching live configuration capability.
constexpr int SUPPORTED_VERSION = 16;

}  // namespace ton
