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

    Copyright 2017-2020 Telegram Systems LLP
*/
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/bigexp.h"
#include "common/bigint.hpp"
#include "common/bitstring.h"
#include "common/global-version.h"
#include "common/refcnt.hpp"
#include "common/refint.h"
#include "common/util.h"
#include "block/block-parse.h"
#include "block/transaction.h"
#include "crypto/Ed25519.h"
#include "td/utils/crypto.h"
#include "td/utils/misc.h"
#include "td/utils/tests.h"
#include "vm/boc.h"
#include "vm/cells.h"
#include "vm/cellslice.h"

static std::stringstream create_ss() {
  std::stringstream ss;
  ss.imbue(std::locale::classic());
  ss.setf(std::ios_base::fixed, std::ios_base::floatfield);
  ss.precision(6);
  return ss;
}
static std::stringstream os = create_ss();

void show_total_cells(std::ostream& stream) {
  stream << "total cells = " << vm::DataCell::get_total_data_cells() << std::endl;
}

TEST(Cells, simple) {
  os = create_ss();
  using namespace td::literals;
  vm::CellBuilder cb1, cb2;
  cb1.store_bytes("Hello, ", 7).reserve_slice(48) = td::BitSlice{(const unsigned char*)"world!", 48};
  cb2.store_bits(td::BitSlice{(const unsigned char*)"\xd0", 4})
      .store_long(17239, 16)
      .store_long(-17, 11)
      .store_long(1000000239, 32)
      .store_long(1000000239LL * 1000000239)
      .store_int256("-1000000000000000000000000239"_i256, 91);
  cb1.store_ref(cb2.finalize_copy());
  show_total_cells(os);
  cb2.store_bytes("<->", 3);
  td::Ref<vm::DataCell> c1{cb1.finalize_copy()}, c2{cb2.finalize_copy()};
  unsigned char hbuff[vm::Cell::hash_bytes];
  os << "cb1 = " << cb1 << "; hash=" << td::buffer_to_hex(td::Slice(cb1.compute_hash(hbuff), 32)) << "; c1 = " << *c1
     << std::endl;
  os << "cb2 = " << cb2 << "; hash=" << td::buffer_to_hex(td::Slice(cb2.compute_hash(hbuff), 32)) << "; c2 = " << *c2
     << std::endl;
  show_total_cells(os);

  vm::CellSlice cr1(c1);
  cr1.dump(os);
  os << "fetch_octet() = " << cr1.fetch_octet() << std::endl;
  cr1.dump(os);
  os << "fetch_octet() = " << cr1.fetch_octet() << std::endl;
  cr1.dump(os);
  os << "fetch_octet() = " << cr1.fetch_octet() << std::endl;
  cr1.dump(os);
  os << "fetch_octet() = " << cr1.fetch_octet() << std::endl;
  cr1.dump(os);
  os << "fetch_ref()=" << td::buffer_to_hex(cr1.prefetch_ref()->get_hash().as_slice()) << std::endl;

  vm::CellSlice cr(vm::NoVm(), cr1.fetch_ref());
  cr.dump(os);
  os << "prefetch_ulong(4)=" << cr.prefetch_ulong(4) << std::endl;
  cr.dump(os);
  os << "fetch_ulong(4)=" << cr.fetch_ulong(4) << std::endl;
  cr.dump(os);
  os << "fetch_long(16)=" << cr.fetch_long(16) << std::endl;
  cr.dump(os);
  os << "prefetch_long(11)=" << cr.prefetch_long(11) << std::endl;
  cr.dump(os);
  os << "fetch_int256(11)=" << cr.fetch_int256(11) << std::endl;
  cr.dump(os);
  os << "fetch_long(32)=" << cr.fetch_long(32) << std::endl;
  cr.dump(os);
  os << "prefetch_long(64)=" << cr.prefetch_long(64) << std::endl;
  cr.dump(os);
  os << "fetch_long(64)=" << cr.fetch_long(64) << std::endl;
  cr.dump(os);
  os << "prefetch_int256(91)=" << cr.prefetch_int256(91) << std::endl;
  cr.dump(os);
  os << "fetch_int256(91)=" << cr.fetch_int256(91) << std::endl;
  cr.dump(os);
  os << "fetch_long(24)=" << cr.fetch_long(24) << std::endl;
  cr.dump(os);
  cr.clear();

  REGRESSION_VERIFY(os.str());
}

void test_two_bitstrings(const td::BitSlice& bs1, const td::BitSlice& bs2) {
  using td::to_binary;
  using td::to_hex;
  os << "bs1 = " << bs1.to_binary() << " = " << bs1.to_hex() << std::endl;
  os << "bs2 = " << to_binary(bs2) << " = " << to_hex(bs2) << std::endl;
  td::BitString st{bs1};
  //td::BitString st;
  //st.append(bs1);
  os << "st = " << to_binary(st) << " = " << to_hex(st) << std::endl;
  st.append(bs2);
  os << "st = " << to_binary(st) << " = " << to_hex(st) << std::endl;
  ASSERT_EQ(to_binary(st), to_binary(bs1) + to_binary(bs2));
  auto bs3 = st.subslice(bs1.size(), bs2.size());
  os << "bs3 = " << to_binary(bs3) << " = " << to_hex(bs3) << std::endl;
  ASSERT_EQ(to_binary(bs3), to_binary(bs2));
  ASSERT_EQ(to_hex(bs3), to_hex(bs2));
  bs1.dump(os);
  bs2.dump(os);
  bs3.dump(os);
  std::string bs2_bin = to_binary(bs2);
  for (unsigned i = 0; i <= bs2.size(); i++) {
    for (unsigned j = 0; j <= bs2.size() - i; j++) {
      auto bs4 = bs2.subslice(i, j);
      auto bs5 = bs3.subslice(i, j);
      if (!(to_binary(bs4) == to_binary(bs5) && to_hex(bs4) == to_hex(bs5) && to_binary(bs4) == bs2_bin.substr(i, j))) {
        bs4.dump(os);
        bs5.dump(os);
        os << "bs2.subslice(" << i << ", " << j << ") = " << to_binary(bs4) << " = " << to_hex(bs4) << std::endl;
        os << "bs3.subslice(" << i << ", " << j << ") = " << to_binary(bs5) << " = " << to_hex(bs5) << std::endl;
      }
      ASSERT_EQ(to_binary(bs4), to_binary(bs5));
      ASSERT_EQ(to_hex(bs4), to_hex(bs5));
      ASSERT_EQ(to_binary(bs4), bs2_bin.substr(i, j));
    }
  }
}

void test_one_bitstring(const td::BitSlice& bs) {
  std::string bs_bin = bs.to_binary();
  for (unsigned i1 = 0; i1 <= bs.size(); i1++) {
    for (unsigned j1 = 0; j1 <= bs.size() - i1; j1++) {
      auto bs1 = bs.subslice(i1, j1);
      ASSERT_EQ(bs1.to_binary(), bs_bin.substr(i1, j1));
      for (unsigned i2 = 0; i2 <= bs.size() && i2 < 8; i2++) {
        for (unsigned j2 = 0; j2 <= bs.size() - i2; j2++) {
          os << "(" << i1 << "," << j1 << ")+(" << i2 << "," << j2 << ")" << std::endl;
          auto bs2 = bs.subslice(i2, j2);
          ASSERT_EQ(bs2.to_binary(), bs_bin.substr(i2, j2));
          test_two_bitstrings(bs1, bs2);
        }
      }
    }
  }
}

void test_bitstring_fill(unsigned n, unsigned p, unsigned k) {
  td::BitString bs{n * 2};
  std::string s;
  auto sl1 = td::BitSlice{(const unsigned char*)"\x40", 2};
  for (unsigned i = 0; i < n; i++) {
    bs.append(sl1);
    s += "01";
  }
  os << td::to_binary(bs) << " = " << td::to_hex(bs) << std::endl;
  ASSERT_EQ(td::to_binary(bs), s);
  unsigned q = k %= p;
  for (unsigned i = 0; i < p; i++) {
    unsigned a = (q * n * 2) / p;
    unsigned b = ((q + 1) * n * 2) / p;
    bs.subslice_write(a, b - a) = (q & 1);
    std::fill(s.begin() + a, s.begin() + b, (q & 1) + '0');
    os << "Step " << i << " (" << a << "," << b << "): " << td::to_binary(bs) << " = " << td::to_hex(bs) << std::endl;
    ASSERT_EQ(td::to_binary(bs), s);
    q = (q + k) % p;
  }
  bs.subslice_write(4, 16) = td::BitSlice{(const unsigned char*)"\x69\x96", 16};
  os << td::to_binary(bs) << " = " << td::to_hex(bs) << std::endl;
  std::string t = "0110100110010110";
  std::copy(t.begin(), t.end(), s.begin() + 4);
  ASSERT_EQ(td::to_binary(bs), s);
}

TEST(Bitstrings, main) {
  os = create_ss();
  auto test = td::BitSlice{(const unsigned char*)"test", 32};
  ASSERT_EQ(test.to_hex(), "74657374");
  test_two_bitstrings({(const unsigned char*)"\xf1\xd0", 12}, test);
  test_two_bitstrings({(const unsigned char*)"\x9f", 3}, {(const unsigned char*)"t", 3});
  test_bitstring_fill(17 * 3, 17, 4);
  //test_one_bitstring({(const unsigned char*)"SuperTest", 72});
  REGRESSION_VERIFY(os.str());
}

void test_parse_dec(std::string s) {
  td::BigInt256 x, y;
  os << "s=\"" << s << "\"" << std::endl;
  x.parse_dec_slow(s);
  y.parse_dec(s);
  x.dump(os);
  y.dump(os);
  ASSERT_TRUE(x == y);
  std::string s1 = x.to_dec_string();
  os << s1 << std::endl;
  ASSERT_EQ(s, s1);
  std::string s2 = x.to_hex_string();
  os << s2 << std::endl;
  std::string s3 = x.to_hex_string_slow();
  os << s3 << std::endl;
  ASSERT_EQ(s2, s3);
}

void test_pow2(int exponent) {
  td::BigInt256 x;
  x.set_pow2(exponent);
  os << "2^" << exponent << " = " << x.to_dec_string() << " = 0x" << x.to_hex_string() << std::endl;
  x.dump(os);
}

void test_fits(const td::BigInt256& x) {
  int m = 0, n = 0;
  const int limit = 300;
  os << "x=" << x.to_dec_string() << "; log2(|x|)=" << std::log2(std::abs(x.to_double())) << std::endl;
  x.dump(os);
  while (m < limit && !x.unsigned_fits_bits(m)) {
    m++;
  }
  for (int i = m; i < limit; i++) {
    ASSERT_TRUE(x.unsigned_fits_bits(i));
  }
  int su = x.bit_size(false);
  while (n < limit && !x.signed_fits_bits(n)) {
    n++;
  }
  for (int i = n; i < limit; i++) {
    ASSERT_TRUE(x.signed_fits_bits(i));
  }
  int ss = x.bit_size();
  os << "x=" << x.to_dec_string() << "=0x" << x.to_hex_string() << "; x=" << x.to_double()
     << "; log2(|x|)=" << std::log2(std::abs(x.to_double())) << "; unsigned: " << m << "=" << su
     << " bits; signed: " << n << "=" << ss << " bits" << std::endl;
  ASSERT_TRUE(su == m || (su == 0x7fffffff && m == limit));
  ASSERT_EQ(ss, n);
  ASSERT_EQ(x.to_hex_string(), x.to_hex_string_slow());
  td::BigInt256 y, z;
  ASSERT_TRUE(y.parse_hex(x.to_hex_string()) && y == x);
  ASSERT_TRUE(z.parse_dec(x.to_dec_string()) && z == x);
}

void test_divmod(const td::BigInt256& x, const td::BigInt256& y) {
  td::BigInt256 q, r(x);
  os << "x = " << x << " = ";
  x.dump(os);
  os << "y = " << y << " = ";
  y.dump(os);
  if (!r.mod_div_bool(y, q)) {
    os << "division error!\n";
    ASSERT_TRUE(0);
  } else {
    q.dump(os);
    r.dump(os);
    if (!q.normalize_bool() || !r.normalize_bool()) {
      os << "cannot normalize q or r!\n";
      ASSERT_TRUE(0);
    } else {
      os << "q = " << q << "; r = " << r << std::endl;
      if (y.sgn() > 0) {
        ASSERT_TRUE(r.sgn() >= 0);
        ASSERT_TRUE(r.cmp(y) < 0);
      } else {
        ASSERT_TRUE(r.sgn() <= 0);
        ASSERT_TRUE(r.cmp(y) > 0);
      }
      r.add_mul(q, y);
      ASSERT_TRUE(r.normalize() == x);
    }
  }
}

void test_export_int(const td::BigInt256& x, bool sgnd = true) {
  os << "x = " << x.to_hex_string() << std::endl;
  int bad = 0, ok = 0;
  for (int i = 1; i <= 33; i++) {
    unsigned char buff[33];
    std::memset(buff, 0xcc, sizeof(buff));
    if (!x.export_bytes(buff, i, sgnd)) {
      ASSERT_EQ(bad, i - 1);
      bad = i;
      continue;
    } else if (++ok < 5) {
      if (bad == i - 1) {
        os << "export(" << bad << ", " << sgnd << ") = (bad)" << std::endl;
      }
      os << "export(" << i << ", " << sgnd << ") =";
      char tmp[33 * 3 + 1];
      for (int j = 0; j < i; j++) {
        sprintf(tmp + 3 * j, " %02x", buff[j]);
      }
      os << tmp << std::endl;
      td::BigInt256 y;
      ASSERT_TRUE(y.import_bytes(buff, i, sgnd));
      os << "import() = " << y.to_hex_string() << std::endl;
      ASSERT_TRUE(!x.cmp_un(y));
    }
  }
  if (!ok) {
    os << "export(" << bad << ", " << sgnd << ") = (bad)" << std::endl;
  }
}

TEST(Bigint, main) {
  os = create_ss();
  using namespace td::literals;
  td::BigInt256 x, y, z;
  test_parse_dec("0");
  test_parse_dec("1");
  test_parse_dec("-1");
  test_parse_dec("123");
  test_parse_dec("-239");
  test_parse_dec("-115792089237316195423570985008687907853269984665640564039457584007913129639936");
  test_parse_dec("115792089237316195423570985008687907853269984665640564039457584007913129639935");
  test_parse_dec("143126893554044595713052252685501316785002612509329899766666973726012466208042");
  test_parse_dec("100000000000000000000000000000000000000000000000000000000000000000000000000001");
  x.parse_dec("11111111111111111111111111111111111111111111111111111111111111111111111111111");
  y.parse_dec("22222222222222222222222222222222222");
  x += y;
  os << x.to_dec_string() << std::endl;
  x -= y;
  os << x.to_dec_string() << std::endl;
  x -= y;
  os << x.to_dec_string() << std::endl;
  y -= x;
  os << y.to_dec_string() << std::endl;
  y += x;
  os << x.to_dec_string() << std::endl;
  x.parse_dec("10000000000000000000000000000001");
  y.parse_dec("11111111111111111111111111111111");
  z.add_mul(x, y);
  os << x.to_dec_string() << " * " << y.to_dec_string() << " = " << z.to_dec_string() << std::endl;
  test_pow2(0);
  test_pow2(1);
  test_pow2(54);
  test_pow2(55);
  test_pow2(56);
  test_pow2(57);
  test_pow2(4 * 56 - 2);
  test_pow2(4 * 56 - 1);
  test_pow2(4 * 56);
  test_pow2(4 * 56 + 1);
  test_pow2(255);
  test_pow2(256);
  test_fits("1111111111111111111111111111"_i256);
  test_fits(
      "0000000000000000000000000000000000000000000000000000000000000000000000000000000000ffffffffffffffffffffffffffff"_x256);
  for (int i = 10; i >= -10; --i) {
    test_fits(td::BigInt256(i));
  }
  test_export_int("10000000000000000000000000000000000000"_i256);
  for (int k = 127; k <= 129; k++) {
    x.set_pow2(k).add_tiny(-1);
    test_export_int(x, true);
    test_export_int(x, false);
    x.add_tiny(1);
    test_export_int(x, true);
    test_export_int(x, false);
    x.add_tiny(1);
    test_export_int(x, true);
    test_export_int(x, false);
    x.negate();
    test_export_int(x, true);
    test_export_int(x, false);
    x.add_tiny(1);
    test_export_int(x, true);
    test_export_int(x, false);
    x.add_tiny(1);
    test_export_int(x, true);
    test_export_int(x, false);
  }
  for (x = 1, y.set_pow2(256).divmod_tiny(3); x.cmp(y) < 0; x.mul_tiny(3).normalize()) {
    test_export_int(x, true);
    x.negate();
    test_export_int(x, true);
    x.negate();
  }
  test_export_int("7fffffffffffffffffffffffffffffff"_x256);
  test_export_int("ffffffffffffffffffffffffffffffff"_x256);
  test_export_int("7fffffffffffffffffffffffffffffff"_x256);
  test_export_int("ffffffffffffffffffffffffffffffff"_x256);
  for (int i = 0; i <= 257; i++) {
    x.set_pow2(i).add_tiny(-3);
    for (int j = -3; j <= 3; j++) {
      x.negate().normalize();
      os << "-2^" << i << "+" << -j << ": ";
      test_fits(x);
      x.negate().normalize();
      os << "2^" << i << "+" << j << ": ";
      test_fits(x);
      x.add_tiny(1);
    }
  }

  for (auto t : {"fffffffffffffffffffffffffffffffffffffff"_x256, td::BigInt256{-1},
                 "123456789abcdef0123456789abcdef0123456789abcdef"_x256, "-8000000000000000000000000001"_x256}) {
    for (int i = 0; i <= 256; i++) {
      (x = t).mod_pow2(i).dump(os);
      os << "mod 2^" << i << " : " << x.to_hex_string() << std::endl;
    }
  }

  test_divmod(x.set_pow2(224), "10000000000000"_i256);
  test_divmod(x.set_pow2(256), "100000000000000000000000000000000000000000"_i256);
  test_divmod(x.set_pow2(256), "100000000000000000000000000000000000000000000"_i256);
  test_divmod(x.set_pow2(80), "-100000000000000000000000000000000000000000000"_i256);
  test_divmod(x.set_pow2(256), y.set_pow2(128).add_tiny(-1));
  test_divmod(x.set_pow2(224), y.set_pow2(112).add_tiny(-1));
  test_divmod(x.set_pow2(222), y.set_pow2(111).add_tiny(-1));
  test_divmod(td::BigInt256(-1), y.set_pow2(256));
  test_divmod("10000000000000000000000000000000000000000000000000000000000000000"_i256,
              "142857142857142857142857142857142857"_i256);
  test_divmod("100000000"_i256, "-253"_i256);
  test_divmod("-100000000"_i256, "-253"_i256);
  test_divmod("-100000000"_i256, "253"_i256);

  test_divmod(x.set_pow2(222), td::BigInt256{std::numeric_limits<td::BigInt256::word_t>::min()});
  test_divmod(x.set_pow2(222).negate(), td::BigInt256{std::numeric_limits<td::BigInt256::word_t>::min()});
  REGRESSION_VERIFY(os.str());
}

TEST(RefInt, main) {
  os = create_ss();
  using namespace td::literals;
  auto x = "10000000000000000000000"_ri256;
  td::RefInt256 y{true, -239}, z{false};
  auto v = x + y;
  os << x << " + " << y << " = " << x + y << std::endl;
  os << x << " - " << y << " = " << x - y << std::endl;
  os << x << " * " << y << " = " << x * y << std::endl;
  os << x << " / " << y << " = " << x / y << std::endl;
  os << x << " % " << y << " = " << x % y << std::endl;
  os << x << " + " << y << " = " << x + y << std::endl;
  os << "10000000000000000000000000000000000000000"_ri256 / "27182818284590"_ri256 << std::endl;
  {
    auto w(x + y);
    z = w;
  }
  os << "(x-y)*(x+y) = " << (x - y) * (x + y) << std::endl;
  os << "z = " << z << std::endl;
  z = x;
  x += y;
  os << "new x = " << x << " = 0x" << hex_string(x) << std::endl;
  os << "z = (old x) = " << std::move(z) << std::endl;
  os << "x + y = " << std::move(x) + std::move(y) << std::endl;
  z = "10000000000000000000000000000000000000000000000000000000000000000000000"_ri256;
  //z = td::RefInt256{true}
  //z.unique_write()->set_pow2(256);
  x = td::RefInt256{true, 0};
  int i = 1;
  while (z->sgn() > 0) {
    x += z;
    z.write().add_tiny(i >> 1).divmod_tiny(i);
    ++i;
  }
  x.write().normalize();
  os << x << " = " << hex_string(x) << std::endl;
  REGRESSION_VERIFY(os.str());
}

TEST(crc16, main) {
  os = create_ss();
  std::string s = "EMSI_FCK";
  unsigned crc16 = td::crc16(td::Slice{s});
  os << "s = `" << s << "`; crc16 = " << std::hex << crc16 << std::dec << std::endl;
  REGRESSION_VERIFY(os.str());
}

TEST(base64, main) {
  os = create_ss();
  std::vector<std::string> arr = {"TEST STRING NUMBER ONE", "TEST STRING NUMBER FOUR", "TEST STRING NUMBER THREE"};
  for (std::string s : arr) {
    std::string t = td::str_base64_encode(s);
    std::string u = td::str_base64_decode(t);
    os << "`" << s << "` -> `" << t << "` -> `" << u << "`" << std::endl;
    os << (s == u) << std::endl;
  }
  std::string s;
  int k = 0;
  for (int i = 0; i < 1024; i++) {
    s.push_back((char)(k >> 8));
    k = 69069 * k + 1;
  }
  std::string t = td::str_base64_encode(s);
  std::string u = td::str_base64_decode(t, true);
  os << t << std::endl;
  os << (s == u) << std::endl;
  t = td::str_base64_encode(s, true);
  u = td::str_base64_decode(t, true);
  os << t << std::endl;
  os << (s == u) << std::endl;
  u = td::sha256(td::Slice{s});
  for (int i = 0; i < 32; i++) {
    os << std::hex << ((u[i] >> 4) & 15) << (u[i] & 15);
  }
  os << std::dec << std::endl;
  REGRESSION_VERIFY(os.str());
}

void check_bits256_scan(std::ostream& stream, td::Bits256 a, td::Bits256 b) {
  auto c = a ^ b;
  auto bit = c.count_leading_zeroes();
  auto bit2 = a.count_matching(b);
  // stream << a.to_hex() << " and " << b.to_hex() << " match in " << bit << " or " << bit2 << " first bits" << std::endl;
  // std::cerr << a.to_hex() << " and " << b.to_hex() << " match in " << bit << " or " << bit2 << " first bits (a XOR b = " << c.to_hex() << ")" << std::endl;
  CHECK((int)bit >= 0 && bit <= 256);
  for (td::uint32 i = 0; i < bit; i++) {
    CHECK(a[i] == b[i]);
  }
  CHECK(bit == 256 || a[bit] != b[bit]);
  CHECK(bit == bit2);
}

void check_bits_scan(std::ostream& stream, td::ConstBitPtr a, bool value) {
  auto bit = (unsigned)a.scan(value, 256);
  CHECK((int)bit >= 0 && bit <= 256);
  for (td::uint32 i = 0; i < bit; i++) {
    CHECK(a[i] == value);
  }
  CHECK(bit == 256 || a[bit] != value);
}

TEST(bits256_scan, main) {
  os = create_ss();
  td::Bits256 a, b;
  int k = 0;
  unsigned char r[1024];
  for (auto& c : r) {
    c = (k & 0x80) ? (unsigned char)(k >> 8) : 0;
    k = 69069 * k + 1;
  }
  for (k = 0; k < 32; k++) {
    a = td::ConstBitPtr{r + 32 * k};
    for (int j = 0; j < 32; j++) {
      b = td::ConstBitPtr{r + 32 * j};
      check_bits256_scan(os, a, b);
    }
    b = a;
    unsigned i = r[7 + k];
    b[i] = b[i] ^ true;
    check_bits256_scan(os, a, b);
  }
  for (k = 0; k < 256; k++) {
    check_bits_scan(os, td::ConstBitPtr{r} + k, false);
    check_bits_scan(os, td::ConstBitPtr{r} + k, true);
  }
  os << "bits256_scan test OK";
  REGRESSION_VERIFY(os.str());
}

bool check_exp(std::ostream& stream, const td::NegExpBinTable& tab, double x) {
  long long xx = llround(x * (1LL << 52));
  td::BigInt256 yy;
  if (!tab.nexpf(yy, -xx, 52)) {
    stream << "cannot compute exp(" << x << ") = exp(" << xx << " * 2^(-52))" << std::endl;
    return false;
  }
  double y = yy.to_double() * exp2(-252);
  double y0 = exp(x);
  bool ok = (fabs(y - y0) < 1e-15);
  if (!ok) {
    stream << "exp(" << x << ") = exp(" << xx << " * 2^(-52)) = " << yy << " / 2^252 = " << y << " (correct value is "
           << y0 << ") " << (ok ? "match" : "incorrect") << std::endl;
  }
  return ok;
}

TEST(bigexp, main) {
  os = create_ss();
  td::NegExpBinTable tab(252, 32, -128);
  bool ok = true;
  if (!tab.is_valid()) {
    os << "cannot initialize td::NegExpBinTable(252, 32, -128)" << std::endl;
    ok = false;
  } else {
    // for (int i = -128; i < 32; i++) {
    //  os << "exp(-2^" << i << ") = " << tab.exp_pw2_ref(i) << " / 2^252 = " << tab.exp_pw2_ref(i)->to_double() * exp2(-252) << " (correct value is " << exp(-exp2(i)) << ")" << std::endl;
    // }
    ok &= check_exp(os, tab, -2.39);
    ok &= check_exp(os, tab, 0);
    ok &= check_exp(os, tab, -1);
    ok &= check_exp(os, tab, -2);
    ok &= check_exp(os, tab, -16);
    ok &= check_exp(os, tab, -17);
    ok &= check_exp(os, tab, -0.5);
    ok &= check_exp(os, tab, -0.25);
    ok &= check_exp(os, tab, -3.1415926535);
    ok &= check_exp(os, tab, -1e-9);
  }
  if (ok) {
    os << "bigexp test OK\n";
  } else {
    os << "bigexp test FAILED\n";
  }
  REGRESSION_VERIFY(os.str());
}

bool check_intexp(std::ostream& stream, td::uint64 x, unsigned k, td::uint64 yc = 0) {
  td::uint64 y = td::umulnexps32(x, k);
  long long delta = (long long)(y - yc);
  bool ok = (y <= x && std::abs(delta) <= 1);
  if (!ok) {
    stream << x << "*exp(-" << k << "/65536) = " << y << " (correct value " << yc << ", delta = " << delta << ")"
           << std::endl;
  }
  return ok;
}

TEST(uint64_exp, main) {
  os = create_ss();
  bool ok = true;
  ok &= check_intexp(os, 3167801306015831286, 4003, 2980099890648636481);
  ok &= check_intexp(os, 1583900653007915643, 4003, 1490049945324318240);
  ok &= check_intexp(os, 9094494907266047891, 17239, 6990995826652297465);
  ok &= check_intexp(os, 5487867407433215099, 239017, 143048684491504152);
  ok &= check_intexp(os, 46462010749955243, 239017, 1211095134625318);  // up
  ok &= check_intexp(os, 390263500024095125, 2700001, 1);
  ok &= check_intexp(os, 390263500024095124, 2700001, 1);
  ok &= check_intexp(os, std::numeric_limits<td::uint64>::max(), 2952601, 1);
  ok &= check_intexp(os, std::numeric_limits<td::uint64>::max(), 2952696, 1);
  ok &= check_intexp(os, std::numeric_limits<td::uint64>::max(), 2952697, 0);
  ok &= check_intexp(os, std::numeric_limits<td::uint64>::max(), 2952800, 0);
  ok &= check_intexp(os, std::numeric_limits<td::uint64>::max(), 295269700, 0);
  ok &= check_intexp(os, std::numeric_limits<td::uint64>::max(), 2000018, 1028453);
  ok &= check_intexp(os, 1ULL << 60, 2770991, 1);
  ok &= check_intexp(os, 1ULL << 60, 2770992, 0);
  if (ok) {
    os << "uint64_exp test OK\n";
  } else {
    os << "uint64_exp test FAILED\n";
  }
  REGRESSION_VERIFY(os.str());
}

TEST(NativeStateEngine, compact_batch_v2) {
  auto source_key = td::Ed25519::generate_private_key().move_as_ok();
  auto destination_key = td::Ed25519::generate_private_key().move_as_ok();
  auto source_public = source_key.get_public_key().move_as_ok().as_octet_string();
  auto destination_public = destination_key.get_public_key().move_as_ok().as_octet_string();

  block::NativeTransfer transfer;
  transfer.src.as_slice().copy_from(source_public);
  transfer.dst.as_slice().copy_from(destination_public);
  transfer.amount = 100;
  transfer.fee = 3;
  transfer.nonce = 7;
  transfer.valid_until = std::numeric_limits<ton::UnixTime>::max();
  transfer.signature = source_key.sign(transfer.signing_payload()).move_as_ok().as_slice().str();
  ASSERT_TRUE(transfer.verify_signature().is_ok());
  vm::CellBuilder external_builder;
  ASSERT_TRUE(transfer.store_external(external_builder));
  auto external_root = external_builder.finalize();
  ASSERT_EQ(transfer.external_hash().move_as_ok(), ton::Bits256{external_root->get_hash().bits()});

  block::NativeTransferBatch batch;
  batch.version = 2;
  batch.entries.push_back({transfer, 0, 0});
  vm::CellBuilder builder;
  ASSERT_TRUE(batch.store(builder));
  auto unpacked = block::NativeTransferBatch::unpack(builder.finalize()).move_as_ok();
  ASSERT_EQ(unpacked.version, 2);
  ASSERT_EQ(unpacked.entries.size(), 1u);
  ASSERT_EQ(unpacked.entries[0].debit_lt, 0u);
  ASSERT_EQ(unpacked.entries[0].credit_lt, 0u);
  ASSERT_EQ(unpacked.entries[0].transfer.signature, transfer.signature);

  block::NativeTransferStateInput input{
      .transfer = &unpacked.entries[0].transfer,
      .src_balance = 1000,
      .src_nonce = 7,
      .src_status = block::Account::acc_uninit,
      .src_is_native = true,
      .dst_balance = 50,
      .dst_status = block::Account::acc_uninit,
      .dst_is_native = true,
  };
  std::vector<block::NativeTransferStateInput> inputs(128, input);
  auto results = block::execute_native_transfer_states_parallel(inputs, transfer.valid_until - 1, true, 4);
  ASSERT_EQ(results.size(), inputs.size());
  for (const auto& result : results) {
    ASSERT_EQ(result.code, block::NativeTransferStateResult::ok);
    ASSERT_EQ(result.src_balance, 897u);
    ASSERT_EQ(result.src_nonce, 8u);
    ASSERT_EQ(result.dst_balance, 150u);
  }
}

TEST(NativeStateEngine, installs_prevalidated_native_account_cell) {
  ton::StdSmcAddress address;
  address.clear();
  block::Account account{ton::basechainId, address.cbits()};
  account.status = block::Account::acc_uninit;

  vm::CellBuilder builder;
  td::Ref<vm::Cell> prepared;
  ASSERT_TRUE(builder.store_long_bool(1, 2));
  ASSERT_TRUE(builder.store_ulong_rchk_bool(1234, 64));
  ASSERT_TRUE(builder.store_ulong_rchk_bool(17, 64));
  ASSERT_TRUE(builder.store_ulong_rchk_bool(3, 8));
  ASSERT_TRUE(builder.finalize_to(prepared));
  ASSERT_TRUE(block::gen::t_Account.validate_ref(prepared));

  auto prepared_hash = prepared->get_hash();
  ASSERT_TRUE(account.set_prevalidated_native_state(prepared, 1234, 17, 3));
  ASSERT_EQ(account.total_state->get_hash(), prepared_hash);
  ASSERT_TRUE(account.is_native);
  ASSERT_EQ(account.native_balance_uint64().value(), 1234u);
  ASSERT_EQ(account.native_nonce, 17u);
  ASSERT_EQ(account.native_flags, 3u);

  block::Account mismatch{ton::basechainId, address.cbits()};
  mismatch.status = block::Account::acc_uninit;
  ASSERT_TRUE(!mismatch.set_prevalidated_native_state(std::move(prepared), 1235, 17, 3));
  ASSERT_TRUE(!mismatch.is_native);
}

TEST(NativeStateEngine, parallel_native_account_state_cells_preserve_canonical_indices) {
  std::vector<block::NativeAccountStateCellInput> inputs;
  inputs.reserve(2'048);
  for (td::uint64 index = 0; index < 2'048; ++index) {
    inputs.push_back({
        .balance = 1000000 + index * 1009,
        .nonce = 7000 + index * 17,
        .flags = static_cast<td::uint8>(index),
    });
  }

  // Compare a serial materialization with two bounded multi-worker passes.
  // The vectors must stay index-aligned even though workers take items in a
  // nondeterministic order.
  auto serial = block::build_native_account_state_cells_parallel(inputs, 1);
  auto parallel = block::build_native_account_state_cells_parallel(inputs, 4);
  auto repeated = block::build_native_account_state_cells_parallel(inputs, 4);
  ASSERT_EQ(serial.size(), inputs.size());
  ASSERT_EQ(parallel.size(), inputs.size());
  ASSERT_EQ(repeated.size(), inputs.size());
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    ASSERT_TRUE(!serial[index].is_null());
    ASSERT_TRUE(!parallel[index].is_null());
    ASSERT_TRUE(!repeated[index].is_null());
    ASSERT_TRUE(block::gen::t_Account.validate_ref(parallel[index]));
    ASSERT_TRUE(block::tlb::t_Account.validate_ref(parallel[index]));
    ASSERT_EQ(serial[index]->get_hash(), parallel[index]->get_hash());
    ASSERT_EQ(parallel[index]->get_hash(), repeated[index]->get_hash());

    block::gen::Account::Record_account_native decoded;
    auto state = vm::load_cell_slice(parallel[index]);
    ASSERT_TRUE(tlb::unpack_exact(state, decoded));
    ASSERT_EQ(decoded.balance, inputs[index].balance);
    ASSERT_EQ(decoded.nonce, inputs[index].nonce);
    ASSERT_EQ(decoded.flags, static_cast<int>(inputs[index].flags));
  }
}

TEST(NativeStateEngine, parallel_native_account_state_cells_install_prevalidated_accounts) {
  constexpr td::uint64 count = 2'048;
  std::vector<ton::StdSmcAddress> addresses;
  std::vector<block::NativeAccountStateCellInput> inputs;
  std::vector<std::unique_ptr<block::Account>> serial_accounts;
  std::vector<std::unique_ptr<block::Account>> parallel_accounts;
  addresses.reserve(count);
  inputs.reserve(count);
  serial_accounts.reserve(count);
  parallel_accounts.reserve(count);

  for (td::uint64 index = 0; index < count; ++index) {
    std::string bytes(32, '\0');
    for (std::size_t byte = 0; byte < sizeof(index); ++byte) {
      bytes[bytes.size() - 1 - byte] = static_cast<char>(index >> (byte * 8));
    }
    ton::StdSmcAddress address;
    address.as_slice().copy_from(bytes);
    addresses.push_back(address);
    inputs.push_back({
        .balance = 1'000'000 + index * 1009,
        .nonce = 7'000 + index * 17,
        .flags = static_cast<td::uint8>(index),
    });

    auto serial = std::make_unique<block::Account>(ton::basechainId, addresses.back().cbits());
    auto parallel = std::make_unique<block::Account>(ton::basechainId, addresses.back().cbits());
    serial->status = block::Account::acc_uninit;
    parallel->status = block::Account::acc_uninit;
    ASSERT_TRUE(serial->set_native_state(inputs.back().balance, inputs.back().nonce, inputs.back().flags));
    serial_accounts.push_back(std::move(serial));
    parallel_accounts.push_back(std::move(parallel));
  }

  // This mirrors validator replay: immutable cells are prepared in parallel,
  // then installed in a fixed order. The result must exactly match serial
  // Account::set_native_state for every independent final state.
  auto prepared = block::build_native_account_state_cells_parallel(inputs, 4);
  ASSERT_EQ(prepared.size(), inputs.size());
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    ASSERT_TRUE(!prepared[index].is_null());
    ASSERT_TRUE(parallel_accounts[index]->set_prevalidated_native_state(std::move(prepared[index]), inputs[index].balance,
                                                                         inputs[index].nonce, inputs[index].flags));
    ASSERT_EQ(serial_accounts[index]->total_state->get_hash(), parallel_accounts[index]->total_state->get_hash());
    ASSERT_TRUE(parallel_accounts[index]->is_native);
    ASSERT_EQ(parallel_accounts[index]->native_balance_uint64().value(), inputs[index].balance);
    ASSERT_EQ(parallel_accounts[index]->native_nonce, inputs[index].nonce);
    ASSERT_EQ(parallel_accounts[index]->native_flags, inputs[index].flags);
  }
}

TEST(NativeStateEngine, preflighted_shard_accounts_root_is_canonical_after_ordinary_corrections) {
  auto address = [](unsigned char suffix) {
    ton::StdSmcAddress result;
    std::string bytes(32, '\0');
    bytes.back() = static_cast<char>(suffix);
    result.as_slice().copy_from(bytes);
    return result;
  };
  auto shard_account = [](td::uint64 balance, td::uint64 nonce, td::uint64 last_lt) {
    vm::CellBuilder state_builder;
    ASSERT_TRUE(state_builder.store_long_bool(1, 2));
    ASSERT_TRUE(state_builder.store_ulong_rchk_bool(balance, 64));
    ASSERT_TRUE(state_builder.store_ulong_rchk_bool(nonce, 64));
    ASSERT_TRUE(state_builder.store_ulong_rchk_bool(0, 8));
    auto state = state_builder.finalize();
    vm::CellBuilder account_builder;
    ASSERT_TRUE(account_builder.store_ref_bool(std::move(state)));
    ASSERT_TRUE(account_builder.store_bits_bool(ton::Bits256{}));
    ASSERT_TRUE(account_builder.store_ulong_rchk_bool(last_lt, 64));
    return vm::load_cell_slice_ref(account_builder.finalize());
  };

  auto native = address(1);
  auto ordinary = address(2);
  auto restored = address(3);
  auto created = address(4);
  auto deleted = address(5);
  auto restored_absent = address(6);
  vm::AugmentedDictionary initial{256, block::tlb::aug_ShardAccounts};
  ASSERT_TRUE(initial.set(native, shard_account(100, 0, 0)));
  ASSERT_TRUE(initial.set(ordinary, shard_account(200, 0, 0)));
  ASSERT_TRUE(initial.set(restored, shard_account(300, 0, 0)));
  ASSERT_TRUE(initial.set(deleted, shard_account(400, 0, 0)));

  vm::AugmentedDictionary direct{initial};
  ASSERT_TRUE(direct.set(native, shard_account(90, 1, 0)));
  ASSERT_TRUE(direct.set(ordinary, shard_account(210, 0, 2)));
  ASSERT_TRUE(direct.set(created, shard_account(50, 0, 2)));
  ASSERT_TRUE(direct.lookup_delete(deleted).not_null());

  vm::AugmentedDictionary estimator{initial};
  // Native commit installs its exact final value, while ordinary estimation
  // may hold an intermediate value until final combine.
  ASSERT_TRUE(estimator.set(ordinary, shard_account(205, 0, 1)));
  ASSERT_TRUE(estimator.set(restored, shard_account(299, 0, 1)));
  ASSERT_TRUE(estimator.set(created, shard_account(25, 0, 1)));
  ASSERT_TRUE(estimator.lookup_delete(deleted).not_null());
  ASSERT_TRUE(estimator.set(restored_absent, shard_account(1, 0, 1)));
  ASSERT_TRUE(estimator.set(native, shard_account(90, 1, 0)));
  ASSERT_TRUE(estimator.set(ordinary, shard_account(210, 0, 2)));
  ASSERT_TRUE(estimator.set(created, shard_account(50, 0, 2)));
  ASSERT_TRUE(estimator.lookup_delete(deleted).is_null());
  auto original_restored = initial.lookup_extra(restored.cbits(), 256).first;
  ASSERT_TRUE(original_restored.not_null());
  ASSERT_TRUE(estimator.set(restored, std::move(original_restored)));
  ASSERT_TRUE(initial.lookup_extra(restored_absent.cbits(), 256).first.is_null());
  ASSERT_TRUE(estimator.lookup_delete(restored_absent).not_null());

  ASSERT_EQ(estimator.get_root_cell()->get_hash(), direct.get_root_cell()->get_hash());
}

TEST(AugmentedDictionary, bulk_sorted_set_matches_sequential_and_is_atomic) {
  auto address = [](unsigned char first, unsigned char last) {
    ton::StdSmcAddress result;
    std::string bytes(32, '\0');
    bytes.front() = static_cast<char>(first);
    bytes.back() = static_cast<char>(last);
    result.as_slice().copy_from(bytes);
    return result;
  };
  auto shard_account = [](td::uint64 balance, td::uint64 nonce, td::uint64 last_lt) {
    vm::CellBuilder state_builder;
    ASSERT_TRUE(state_builder.store_long_bool(1, 2));
    ASSERT_TRUE(state_builder.store_ulong_rchk_bool(balance, 64));
    ASSERT_TRUE(state_builder.store_ulong_rchk_bool(nonce, 64));
    ASSERT_TRUE(state_builder.store_ulong_rchk_bool(0, 8));
    auto state = state_builder.finalize();
    vm::CellBuilder account_builder;
    ASSERT_TRUE(account_builder.store_ref_bool(std::move(state)));
    ASSERT_TRUE(account_builder.store_bits_bool(ton::Bits256{}));
    ASSERT_TRUE(account_builder.store_ulong_rchk_bool(last_lt, 64));
    return vm::load_cell_slice_ref(account_builder.finalize());
  };

  // The inputs deliberately vary both near the root and near the leaves, so
  // the update list exercises replacements, inserts, and nested prefixes.
  auto existing_low = address(0x10, 0x01);
  auto existing_mid = address(0x20, 0x02);
  auto existing_high = address(0x80, 0x03);
  auto inserted_low = address(0x00, 0x7f);
  auto inserted_mid_low = address(0x20, 0x01);
  auto inserted_mid_high = address(0x20, 0xf0);
  auto inserted_high = address(0xf0, 0x01);

  vm::AugmentedDictionary initial{256, block::tlb::aug_ShardAccounts};
  ASSERT_TRUE(initial.set(existing_low, shard_account(100, 1, 10)));
  ASSERT_TRUE(initial.set(existing_mid, shard_account(200, 2, 20)));
  ASSERT_TRUE(initial.set(existing_high, shard_account(300, 3, 30)));

  std::vector<vm::AugmentedDictionary::SetManyEntry> updates;
  updates.emplace_back(inserted_low.cbits(), shard_account(11, 1, 101));
  updates.emplace_back(existing_low.cbits(), shard_account(111, 4, 102));
  updates.emplace_back(inserted_mid_low.cbits(), shard_account(22, 2, 103));
  updates.emplace_back(existing_mid.cbits(), shard_account(222, 5, 104));
  updates.emplace_back(inserted_mid_high.cbits(), shard_account(33, 3, 105));
  updates.emplace_back(existing_high.cbits(), shard_account(333, 6, 106));
  updates.emplace_back(inserted_high.cbits(), shard_account(44, 4, 107));

  vm::AugmentedDictionary sequential{initial};
  for (const auto& [key, value] : updates) {
    ASSERT_TRUE(sequential.set(key, 256, value));
  }
  ASSERT_TRUE(sequential.validate_all());

  vm::AugmentedDictionary bulk{initial};
  ASSERT_TRUE(bulk.set_many_sorted(td::as_span(updates)));
  ASSERT_TRUE(bulk.validate_all());
  ASSERT_EQ(bulk.get_root_cell()->get_hash(), sequential.get_root_cell()->get_hash());

  const auto committed_root = bulk.get_root_cell()->get_hash();
  std::vector<vm::AugmentedDictionary::SetManyEntry> unsorted;
  unsorted.emplace_back(existing_low.cbits(), shard_account(500, 1, 200));
  unsorted.emplace_back(inserted_low.cbits(), shard_account(501, 1, 201));
  ASSERT_TRUE(!bulk.set_many_sorted(td::as_span(unsorted)));
  ASSERT_EQ(bulk.get_root_cell()->get_hash(), committed_root);

  std::vector<vm::AugmentedDictionary::SetManyEntry> duplicate;
  duplicate.emplace_back(existing_mid.cbits(), shard_account(600, 1, 300));
  duplicate.emplace_back(existing_mid.cbits(), shard_account(601, 1, 301));
  ASSERT_TRUE(!bulk.set_many_sorted(td::as_span(duplicate)));
  ASSERT_EQ(bulk.get_root_cell()->get_hash(), committed_root);

  std::vector<vm::AugmentedDictionary::SetManyEntry> null_leaf;
  null_leaf.emplace_back(inserted_high.cbits(), td::Ref<vm::CellSlice>{});
  ASSERT_TRUE(!bulk.set_many_sorted(td::as_span(null_leaf)));
  ASSERT_EQ(bulk.get_root_cell()->get_hash(), committed_root);
  ASSERT_TRUE(bulk.validate_all());
}

TEST(AugmentedDictionary, parallel_shard_accounts_bulk_merge_is_canonical_and_atomic) {
  auto shard_account = [](td::uint64 balance, td::uint64 nonce, td::uint64 last_lt) {
    vm::CellBuilder state_builder;
    ASSERT_TRUE(state_builder.store_long_bool(1, 2));
    ASSERT_TRUE(state_builder.store_ulong_rchk_bool(balance, 64));
    ASSERT_TRUE(state_builder.store_ulong_rchk_bool(nonce, 64));
    ASSERT_TRUE(state_builder.store_ulong_rchk_bool(0, 8));
    auto state = state_builder.finalize();
    vm::CellBuilder account_builder;
    ASSERT_TRUE(account_builder.store_ref_bool(std::move(state)));
    ASSERT_TRUE(account_builder.store_bits_bool(ton::Bits256{}));
    ASSERT_TRUE(account_builder.store_ulong_rchk_bool(last_lt, 64));
    return vm::load_cell_slice_ref(account_builder.finalize());
  };
  auto make_address = [](std::size_t index, bool prefix_clustered) {
    ton::StdSmcAddress result;
    std::string bytes(32, '\0');
    if (prefix_clustered) {
      // All entries share the first byte but still fan out below it.
      bytes[0] = static_cast<char>(0x20);
      bytes[1] = static_cast<char>(index >> 8);
      bytes[2] = static_cast<char>(index);
    } else {
      // The leading byte changes regularly, exercising root-level siblings.
      bytes[0] = static_cast<char>(index >> 8);
      bytes[1] = static_cast<char>(index);
      bytes[31] = static_cast<char>(index * 37);
    }
    result.as_slice().copy_from(bytes);
    return result;
  };

  auto run_case = [&](bool prefix_clustered) {
    constexpr std::size_t update_count = 2'048;
    std::vector<ton::StdSmcAddress> addresses;
    addresses.reserve(update_count);
    for (std::size_t index = 0; index < update_count; ++index) {
      addresses.push_back(make_address(index, prefix_clustered));
    }

    vm::AugmentedDictionary initial{256, block::tlb::aug_ShardAccounts};
    std::vector<vm::AugmentedDictionary::SetManyEntry> updates;
    updates.reserve(update_count);
    for (std::size_t index = 0; index < update_count; ++index) {
      // Seed half the keys first: the bulk operation must perform both
      // canonical replacements and inserts through every subtree shape.
      if ((index & 1) == 0) {
        ASSERT_TRUE(initial.set(addresses[index], shard_account(10'000 + index, index, 100 + index)));
      }
      updates.emplace_back(addresses[index].cbits(), shard_account(100'000 + index, 1'000 + index, 10'000 + index));
    }

    vm::AugmentedDictionary sequential{initial};
    for (const auto& [key, value] : updates) {
      ASSERT_TRUE(sequential.set(key, 256, value));
    }
    ASSERT_TRUE(sequential.validate_all());
    const auto expected_root = sequential.get_root_cell()->get_hash();

    vm::AugmentedDictionary serial_bulk{initial};
    ASSERT_TRUE(serial_bulk.set_many_sorted(td::as_span(updates)));
    ASSERT_TRUE(serial_bulk.validate_all());
    ASSERT_EQ(serial_bulk.get_root_cell()->get_hash(), expected_root);

    vm::AugmentedDictionary repeated{initial};
    ASSERT_TRUE(repeated.set_many_sorted_parallel(td::as_span(updates), 8));
    ASSERT_TRUE(repeated.validate_all());
    ASSERT_EQ(repeated.get_root_cell()->get_hash(), expected_root);
    for (unsigned workers : {1u, 2u, 4u, 8u}) {
      vm::AugmentedDictionary parallel{initial};
      ASSERT_TRUE(parallel.set_many_sorted_parallel(td::as_span(updates), workers));
      ASSERT_TRUE(parallel.validate_all());
      ASSERT_EQ(parallel.get_root_cell()->get_hash(), expected_root);
      ASSERT_EQ(parallel.get_root_cell()->get_hash(), repeated.get_root_cell()->get_hash());
    }

    auto usage_tree = std::make_shared<vm::CellUsageTree>();
    vm::NewCellStorageStat serial_stat;
    serial_stat.add_proof(serial_bulk.get_root_cell(), usage_tree.get());
    vm::NewCellStorageStat parallel_stat;
    parallel_stat.add_proof(repeated.get_root_cell(), usage_tree.get());
    ASSERT_EQ(serial_stat.get_total_stat(), parallel_stat.get_total_stat());

    auto serial_update = vm::CellBuilder::create_merkle_update(initial.get_root_cell(), serial_bulk.get_root_cell());
    auto parallel_update = vm::CellBuilder::create_merkle_update(initial.get_root_cell(), repeated.get_root_cell());
    ASSERT_TRUE(serial_update.not_null());
    ASSERT_TRUE(parallel_update.not_null());
    ASSERT_EQ(serial_update->get_hash(), parallel_update->get_hash());

    vm::AugmentedDictionary atomic_parallel{initial};
    const auto committed_root = atomic_parallel.get_root_cell()->get_hash();
    std::vector<vm::AugmentedDictionary::SetManyEntry> unsorted;
    unsorted.emplace_back(addresses[1].cbits(), shard_account(200'001, 1, 1));
    unsorted.emplace_back(addresses[0].cbits(), shard_account(200'002, 1, 1));
    ASSERT_TRUE(!atomic_parallel.set_many_sorted_parallel(td::as_span(unsorted), 4));
    ASSERT_EQ(atomic_parallel.get_root_cell()->get_hash(), committed_root);

    std::vector<vm::AugmentedDictionary::SetManyEntry> duplicate;
    duplicate.emplace_back(addresses[2].cbits(), shard_account(200'003, 1, 1));
    duplicate.emplace_back(addresses[2].cbits(), shard_account(200'004, 1, 1));
    ASSERT_TRUE(!atomic_parallel.set_many_sorted_parallel(td::as_span(duplicate), 4));
    ASSERT_EQ(atomic_parallel.get_root_cell()->get_hash(), committed_root);

    std::vector<vm::AugmentedDictionary::SetManyEntry> null_leaf;
    null_leaf.emplace_back(addresses[3].cbits(), td::Ref<vm::CellSlice>{});
    ASSERT_TRUE(!atomic_parallel.set_many_sorted_parallel(td::as_span(null_leaf), 4));
    ASSERT_EQ(atomic_parallel.get_root_cell()->get_hash(), committed_root);
    ASSERT_TRUE(atomic_parallel.validate_all());

    // UsageCell child traversal records mutable proof/accounting state. The
    // parallel entry point must detect that wrapper graph and preserve the
    // serial result rather than letting worker reads touch it concurrently.
    auto tracked_usage_tree = std::make_shared<vm::CellUsageTree>();
    auto tracked_root = vm::UsageCell::create(initial.get_root_cell(), tracked_usage_tree->root_ptr());
    vm::AugmentedDictionary tracked_initial{tracked_root, 256, block::tlb::aug_ShardAccounts};
    vm::AugmentedDictionary tracked_serial{tracked_initial};
    ASSERT_TRUE(tracked_serial.set_many_sorted(td::as_span(updates)));
    vm::AugmentedDictionary tracked_parallel{tracked_initial};
    ASSERT_TRUE(tracked_parallel.set_many_sorted_parallel(td::as_span(updates), 8));
    ASSERT_TRUE(tracked_parallel.validate_all());
    ASSERT_EQ(tracked_parallel.get_root_cell()->get_hash(), tracked_serial.get_root_cell()->get_hash());
  };

  run_case(/*prefix_clustered=*/false);
  run_case(/*prefix_clustered=*/true);
}

TEST(AugmentedDictionary, parallel_bulk_merge_worker_failure_is_atomic) {
  struct WorkerFailingAugmentation final : vm::dict::AugmentationData {
    std::thread::id owner{std::this_thread::get_id()};
    mutable std::atomic<bool> worker_fork_seen{false};

    bool skip_extra(vm::CellSlice&) const override {
      return true;
    }
    bool eval_leaf(vm::CellBuilder&, vm::CellSlice&) const override {
      return true;
    }
    bool eval_fork(vm::CellBuilder&, vm::CellSlice&, vm::CellSlice&) const override {
      if (std::this_thread::get_id() != owner) {
        worker_fork_seen.store(true, std::memory_order_relaxed);
        return false;
      }
      return true;
    }
    bool eval_empty(vm::CellBuilder&) const override {
      return true;
    }
    bool supports_parallel_construction() const override {
      return true;
    }
  } augmentation;
  auto value = [](td::uint64 number) {
    vm::CellBuilder builder;
    ASSERT_TRUE(builder.store_ulong_rchk_bool(number, 16));
    return vm::load_cell_slice_ref(builder.finalize());
  };

  td::BitArray<8> key0{static_cast<long long>(0)};
  td::BitArray<8> key1{static_cast<long long>(64)};
  td::BitArray<8> key2{static_cast<long long>(128)};
  td::BitArray<8> key3{static_cast<long long>(192)};
  vm::AugmentedDictionary initial{8, augmentation};
  ASSERT_TRUE(initial.set(key0, value(1)));
  ASSERT_TRUE(initial.set(key2, value(2)));
  const auto committed_root = initial.get_root_cell()->get_hash();

  std::vector<vm::AugmentedDictionary::SetManyEntry> updates;
  updates.emplace_back(key0.cbits(), value(10));
  updates.emplace_back(key1.cbits(), value(11));
  updates.emplace_back(key2.cbits(), value(12));
  updates.emplace_back(key3.cbits(), value(13));

  bool failed = false;
  bool applied = false;
  try {
    applied = initial.set_many_sorted_parallel(td::as_span(updates), 2);
  } catch (const vm::VmError&) {
    failed = true;
  }
  ASSERT_TRUE(!applied);
  ASSERT_TRUE(failed);
  ASSERT_TRUE(augmentation.worker_fork_seen.load(std::memory_order_relaxed));
  ASSERT_EQ(initial.get_root_cell()->get_hash(), committed_root);
  ASSERT_TRUE(initial.validate_all());
}

TEST(AugmentedDictionary, parallel_bulk_merge_unsafe_update_reuses_prepared_trie) {
  struct CountingAugmentation final : vm::dict::AugmentationData {
    std::thread::id owner{std::this_thread::get_id()};
    mutable std::atomic<unsigned> leaf_evaluations{0};
    mutable std::atomic<bool> worker_fork_seen{false};

    bool skip_extra(vm::CellSlice& cs) const override {
      return cs.advance(16);
    }
    bool eval_leaf(vm::CellBuilder& cb, vm::CellSlice&) const override {
      return cb.store_ulong_rchk_bool(leaf_evaluations.fetch_add(1, std::memory_order_relaxed) + 1, 16);
    }
    bool eval_fork(vm::CellBuilder& cb, vm::CellSlice&, vm::CellSlice&) const override {
      if (std::this_thread::get_id() != owner) {
        worker_fork_seen.store(true, std::memory_order_relaxed);
      }
      return cb.store_ulong_rchk_bool(0, 16);
    }
    bool eval_empty(vm::CellBuilder& cb) const override {
      return cb.store_ulong_rchk_bool(0, 16);
    }
    bool supports_parallel_construction() const override {
      return true;
    }
  } parallel_augmentation, serial_augmentation;

  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto value = [&](td::uint64 number, bool tracked) {
    vm::CellBuilder child_builder;
    ASSERT_TRUE(child_builder.store_ulong_rchk_bool(number, 8));
    td::Ref<vm::Cell> child = child_builder.finalize();
    if (tracked) {
      child = vm::UsageCell::create(std::move(child), usage_tree->root_ptr());
    }
    vm::CellBuilder value_builder;
    ASSERT_TRUE(value_builder.store_ref_bool(std::move(child)));
    return vm::load_cell_slice_ref(value_builder.finalize());
  };

  std::array<td::BitArray<8>, 4> keys{
      td::BitArray<8>{static_cast<long long>(0x00)}, td::BitArray<8>{static_cast<long long>(0x40)},
      td::BitArray<8>{static_cast<long long>(0x80)}, td::BitArray<8>{static_cast<long long>(0xc0)}};

  vm::AugmentedDictionary parallel{8, parallel_augmentation};
  vm::AugmentedDictionary serial{8, serial_augmentation};
  for (std::size_t index = 0; index < keys.size(); ++index) {
    ASSERT_TRUE(parallel.set(keys[index], value(index, /*tracked=*/false)));
    ASSERT_TRUE(serial.set(keys[index], value(index, /*tracked=*/false)));
  }
  ASSERT_EQ(parallel.get_root_cell()->get_hash(), serial.get_root_cell()->get_hash());
  parallel_augmentation.leaf_evaluations.store(0, std::memory_order_relaxed);
  serial_augmentation.leaf_evaluations.store(0, std::memory_order_relaxed);

  // The update values are plain outer DataCells with UsageCell children. The
  // initial tree is safe to inspect, but the prepared update trie is not safe
  // to enter from worker threads. Its serial fallback must retain that first
  // prepared trie: rebuilding it would evaluate this stateful augmentation
  // twice and would let worker forks run if the guard regressed.
  std::vector<vm::AugmentedDictionary::SetManyEntry> updates;
  for (std::size_t index = 0; index < keys.size(); ++index) {
    updates.emplace_back(keys[index].cbits(), value(10 + index, /*tracked=*/true));
  }
  ASSERT_TRUE(parallel.set_many_sorted_parallel(td::as_span(updates), 2));
  ASSERT_EQ(parallel_augmentation.leaf_evaluations.load(std::memory_order_relaxed), keys.size());
  ASSERT_TRUE(!parallel_augmentation.worker_fork_seen.load(std::memory_order_relaxed));
  ASSERT_TRUE(serial.set_many_sorted(td::as_span(updates)));
  ASSERT_EQ(serial_augmentation.leaf_evaluations.load(std::memory_order_relaxed), keys.size());
  ASSERT_EQ(parallel.get_root_cell()->get_hash(), serial.get_root_cell()->get_hash());
}

TEST(NativeStateEngine, repeated_shard_accounts_checkpoint_rollback_preserves_largest_prefix) {
  auto address = [](unsigned char suffix) {
    ton::StdSmcAddress result;
    std::string bytes(32, '\0');
    bytes.back() = static_cast<char>(suffix);
    result.as_slice().copy_from(bytes);
    return result;
  };
  auto shard_account = [](td::uint64 balance, td::uint64 nonce) {
    vm::CellBuilder state_builder;
    ASSERT_TRUE(state_builder.store_long_bool(1, 2));
    ASSERT_TRUE(state_builder.store_ulong_rchk_bool(balance, 64));
    ASSERT_TRUE(state_builder.store_ulong_rchk_bool(nonce, 64));
    ASSERT_TRUE(state_builder.store_ulong_rchk_bool(0, 8));
    auto state = state_builder.finalize();
    vm::CellBuilder account_builder;
    ASSERT_TRUE(account_builder.store_ref_bool(std::move(state)));
    ASSERT_TRUE(account_builder.store_bits_bool(ton::Bits256{}));
    ASSERT_TRUE(account_builder.store_ulong_rchk_bool(0, 64));
    return vm::load_cell_slice_ref(account_builder.finalize());
  };

  auto source = address(1);
  auto destination = address(2);
  vm::AugmentedDictionary initial{256, block::tlb::aug_ShardAccounts};
  ASSERT_TRUE(initial.set(source, shard_account(100, 0)));
  ASSERT_TRUE(initial.set(destination, shard_account(0, 0)));

  // The first exact checkpoint is the largest fitting prefix.
  vm::AugmentedDictionary accepted_prefix{initial};
  ASSERT_TRUE(accepted_prefix.set(source, shard_account(89, 1)));
  ASSERT_TRUE(accepted_prefix.set(destination, shard_account(10, 0)));
  auto accepted_prefix_hash = accepted_prefix.get_root_cell()->get_hash();

  // A later fragment is staged against a copy. Rejecting its exact hard-limit
  // preflight must leave the accepted prefix root untouched.
  vm::AugmentedDictionary rejected_trial{accepted_prefix};
  ASSERT_TRUE(rejected_trial.set(source, shard_account(78, 2)));
  ASSERT_TRUE(rejected_trial.set(destination, shard_account(20, 0)));
  ASSERT_TRUE(rejected_trial.get_root_cell()->get_hash() != accepted_prefix_hash);
  ASSERT_EQ(accepted_prefix.get_root_cell()->get_hash(), accepted_prefix_hash);

  // Committing that fragment in a candidate with room produces exactly the
  // same canonical root as applying the final values once to the initial
  // dictionary. This is the deferred Account-install invariant.
  vm::AugmentedDictionary committed{accepted_prefix};
  ASSERT_TRUE(committed.set(source, shard_account(78, 2)));
  ASSERT_TRUE(committed.set(destination, shard_account(20, 0)));
  vm::AugmentedDictionary direct{initial};
  ASSERT_TRUE(direct.set(source, shard_account(78, 2)));
  ASSERT_TRUE(direct.set(destination, shard_account(20, 0)));
  ASSERT_EQ(committed.get_root_cell()->get_hash(), direct.get_root_cell()->get_hash());

  // Storage accounting follows the same replaceable-checkpoint rule. Every
  // trial starts with proofs that existed before native processing and adds
  // only its current final ShardAccounts root.
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  vm::CellBuilder pre_native_builder;
  ASSERT_TRUE(pre_native_builder.store_ulong_rchk_bool(0xabc, 12));
  auto pre_native_root = pre_native_builder.finalize();
  vm::NewCellStorageStat pre_native_stat;
  pre_native_stat.add_proof(pre_native_root, usage_tree.get());

  vm::NewCellStorageStat accepted_stat = pre_native_stat;
  accepted_stat.add_proof(accepted_prefix.get_root_cell(), usage_tree.get());
  auto accepted_stat_before_rejected_trial = accepted_stat.get_total_stat();
  vm::NewCellStorageStat rejected_stat = pre_native_stat;
  rejected_stat.add_proof(rejected_trial.get_root_cell(), usage_tree.get());
  ASSERT_EQ(accepted_stat.get_total_stat(), accepted_stat_before_rejected_trial);

  vm::NewCellStorageStat replacement_stat = pre_native_stat;
  replacement_stat.add_proof(committed.get_root_cell(), usage_tree.get());
  vm::NewCellStorageStat direct_stat = pre_native_stat;
  direct_stat.add_proof(direct.get_root_cell(), usage_tree.get());
  ASSERT_EQ(replacement_stat.get_total_stat(), direct_stat.get_total_stat());
  ASSERT_TRUE(replacement_stat.get_total_stat().cells > pre_native_stat.get_total_stat().cells);

  // Additive accounting retains cells that existed only in the previous
  // checkpoint; replacement accounting must not charge them to the block.
  vm::NewCellStorageStat cumulative_stat = accepted_stat;
  cumulative_stat.add_proof(committed.get_root_cell(), usage_tree.get());
  ASSERT_TRUE(cumulative_stat.get_total_stat().cells > replacement_stat.get_total_stat().cells);
}

TEST(NativeStateEngine, native_signature_is_bound_to_zerostate_domain) {
  auto source_key = td::Ed25519::generate_private_key().move_as_ok();
  auto destination_key = td::Ed25519::generate_private_key().move_as_ok();
  block::NativeTransfer transfer;
  transfer.src.as_slice().copy_from(source_key.get_public_key().move_as_ok().as_octet_string());
  transfer.dst.as_slice().copy_from(destination_key.get_public_key().move_as_ok().as_octet_string());
  transfer.amount = 1;
  transfer.nonce = 9;
  transfer.valid_until = std::numeric_limits<ton::UnixTime>::max();

  ton::Bits256 domain_a, domain_b;
  domain_a.as_slice().copy_from(std::string(32, '\x11'));
  domain_b.as_slice().copy_from(std::string(32, '\x22'));
  transfer.signature = source_key.sign(transfer.signing_payload(domain_a)).move_as_ok().as_slice().str();

  ASSERT_TRUE(transfer.verify_signature(domain_a).is_ok());
  ASSERT_TRUE(transfer.verify_signature(domain_b).is_error());
  ASSERT_TRUE(transfer.verify_signature().is_error());
}

TEST(NativeStateEngine, native_transfer_run_is_domain_signed_and_canonical) {
  auto source_key = td::Ed25519::generate_private_key().move_as_ok();
  auto first_destination = td::Ed25519::generate_private_key().move_as_ok();
  auto second_destination = td::Ed25519::generate_private_key().move_as_ok();
  block::NativeTransferRun run;
  run.src.as_slice().copy_from(source_key.get_public_key().move_as_ok().as_octet_string());
  run.first_nonce = 700;
  run.valid_until = std::numeric_limits<ton::UnixTime>::max();
  run.outputs = {
      {.dst = ton::StdSmcAddress{}, .amount = 11, .fee = 2},
      {.dst = ton::StdSmcAddress{}, .amount = 13, .fee = 3},
  };
  run.outputs[0].dst.as_slice().copy_from(first_destination.get_public_key().move_as_ok().as_octet_string());
  run.outputs[1].dst.as_slice().copy_from(second_destination.get_public_key().move_as_ok().as_octet_string());

  ton::Bits256 domain_a, domain_b;
  domain_a.as_slice().copy_from(std::string(32, '\x31'));
  domain_b.as_slice().copy_from(std::string(32, '\x32'));
  run.signature = source_key.sign(run.signing_payload(domain_a)).move_as_ok().as_slice().str();
  ASSERT_TRUE(run.is_valid());
  ASSERT_TRUE(run.verify_signature(domain_a).is_ok());
  ASSERT_TRUE(run.verify_signature(domain_b).is_error());
  ASSERT_TRUE(run.verify_signature().is_error());
  std::vector<const block::NativeTransferRun*> signed_runs{&run};
  ASSERT_TRUE(block::verify_native_transfer_run_signatures_parallel(signed_runs, domain_a).is_ok());
  ASSERT_TRUE(block::verify_native_transfer_run_signatures_parallel(signed_runs, domain_b).is_error());

  vm::CellBuilder first_builder;
  ASSERT_TRUE(run.store_external(first_builder));
  auto first_root = first_builder.finalize();
  auto first_hash = ton::Bits256{first_root->get_hash().bits()};
  ASSERT_EQ(run.external_hash().move_as_ok(), first_hash);
  auto decoded = block::NativeTransferRun::unpack_external(first_root).move_as_ok();
  ASSERT_EQ(decoded.src, run.src);
  ASSERT_EQ(decoded.first_nonce, run.first_nonce);
  ASSERT_EQ(decoded.valid_until, run.valid_until);
  ASSERT_EQ(decoded.outputs.size(), 2u);
  ASSERT_EQ(decoded.outputs[0].dst, run.outputs[0].dst);
  ASSERT_EQ(decoded.outputs[1].amount, run.outputs[1].amount);
  ASSERT_TRUE(decoded.verify_signature(domain_a).is_ok());

  vm::CellBuilder second_builder;
  ASSERT_TRUE(decoded.store_external(second_builder));
  auto second_root = second_builder.finalize();
  ASSERT_EQ(first_root->get_hash(), second_root->get_hash());
  auto first_boc = vm::std_boc_serialize(first_root).move_as_ok();
  auto second_boc = vm::std_boc_serialize(second_root).move_as_ok();
  ASSERT_TRUE(first_boc.as_slice() == second_boc.as_slice());

  decoded.outputs[1].amount++;
  ASSERT_TRUE(decoded.verify_signature(domain_a).is_error());
}

TEST(NativeStateEngine, native_transfer_run_rejects_invalid_ranges_and_output_tree) {
  auto source_key = td::Ed25519::generate_private_key().move_as_ok();
  auto destination_key = td::Ed25519::generate_private_key().move_as_ok();
  block::NativeTransferRun run;
  run.src.as_slice().copy_from(source_key.get_public_key().move_as_ok().as_octet_string());
  run.first_nonce = std::numeric_limits<td::uint64>::max() - 15;
  run.valid_until = std::numeric_limits<ton::UnixTime>::max();
  for (std::size_t index = 0; index < block::NativeTransferRun::max_entries; ++index) {
    block::NativeTransferRunOutput output;
    output.dst.as_slice().copy_from(destination_key.get_public_key().move_as_ok().as_octet_string());
    output.amount = index + 1;
    output.fee = index;
    run.outputs.push_back(std::move(output));
  }
  ton::Bits256 domain;
  domain.as_slice().copy_from(std::string(32, '\x41'));
  run.signature = source_key.sign(run.signing_payload(domain)).move_as_ok().as_slice().str();
  ASSERT_TRUE(run.is_valid());
  run.first_nonce++;
  ASSERT_TRUE(!run.is_valid());
  vm::CellBuilder overflow_builder;
  ASSERT_TRUE(!run.store_external(overflow_builder));
  run.first_nonce--;

  auto signature_cell = [] {
    vm::CellBuilder builder;
    ASSERT_TRUE(builder.store_bytes_bool(std::string(64, '\0')));
    return builder.finalize();
  };
  auto output_leaf = [&] {
    vm::CellBuilder builder;
    ASSERT_TRUE(builder.store_ulong_rchk_bool(block::NativeTransferRun::outputs_leaf_magic, 32));
    ASSERT_TRUE(builder.store_ulong_rchk_bool(1, 5));
    ASSERT_TRUE(builder.store_bits_bool(run.outputs.front().dst));
    ASSERT_TRUE(builder.store_ulong_rchk_bool(1, 64));
    ASSERT_TRUE(builder.store_ulong_rchk_bool(0, 64));
    return builder.finalize();
  };
  vm::CellBuilder zero_count_builder;
  ASSERT_TRUE(zero_count_builder.store_ulong_rchk_bool(block::NativeTransferRun::magic, 32));
  ASSERT_TRUE(zero_count_builder.store_bits_bool(run.src));
  ASSERT_TRUE(zero_count_builder.store_ulong_rchk_bool(0, 64));
  ASSERT_TRUE(zero_count_builder.store_ulong_rchk_bool(run.valid_until, 32));
  ASSERT_TRUE(zero_count_builder.store_ulong_rchk_bool(0, 5));
  ASSERT_TRUE(zero_count_builder.store_ref_bool(signature_cell()));
  ASSERT_TRUE(zero_count_builder.store_ref_bool(output_leaf()));
  ASSERT_TRUE(block::NativeTransferRun::unpack_external(zero_count_builder.finalize()).is_error());

  vm::CellBuilder malformed_leaf_builder;
  ASSERT_TRUE(malformed_leaf_builder.store_ulong_rchk_bool(block::NativeTransferRun::outputs_leaf_magic, 32));
  ASSERT_TRUE(malformed_leaf_builder.store_ulong_rchk_bool(2, 5));
  ASSERT_TRUE(malformed_leaf_builder.store_bits_bool(run.outputs.front().dst));
  ASSERT_TRUE(malformed_leaf_builder.store_ulong_rchk_bool(1, 64));
  ASSERT_TRUE(malformed_leaf_builder.store_ulong_rchk_bool(0, 64));
  vm::CellBuilder malformed_root_builder;
  ASSERT_TRUE(malformed_root_builder.store_ulong_rchk_bool(block::NativeTransferRun::magic, 32));
  ASSERT_TRUE(malformed_root_builder.store_bits_bool(run.src));
  ASSERT_TRUE(malformed_root_builder.store_ulong_rchk_bool(0, 64));
  ASSERT_TRUE(malformed_root_builder.store_ulong_rchk_bool(run.valid_until, 32));
  ASSERT_TRUE(malformed_root_builder.store_ulong_rchk_bool(1, 5));
  ASSERT_TRUE(malformed_root_builder.store_ref_bool(signature_cell()));
  ASSERT_TRUE(malformed_root_builder.store_ref_bool(malformed_leaf_builder.finalize()));
  ASSERT_TRUE(block::NativeTransferRun::unpack_external(malformed_root_builder.finalize()).is_error());

  // The field-level signature must not permit multiple equivalent output-tree
  // layouts. For three outputs the canonical tree is [two-output leaf, one-
  // output leaf]; reverse that split while retaining the signed field sequence.
  block::NativeTransferRun three_run;
  three_run.src = run.src;
  three_run.first_nonce = 17;
  three_run.valid_until = run.valid_until;
  three_run.outputs.assign(run.outputs.begin(), run.outputs.begin() + 3);
  three_run.signature = source_key.sign(three_run.signing_payload(domain)).move_as_ok().as_slice().str();
  auto make_output_leaf = [&](std::size_t start, std::size_t count) {
    vm::CellBuilder builder;
    ASSERT_TRUE(builder.store_ulong_rchk_bool(block::NativeTransferRun::outputs_leaf_magic, 32));
    ASSERT_TRUE(builder.store_ulong_rchk_bool(count, 5));
    for (std::size_t index = start; index < start + count; ++index) {
      const auto& output = three_run.outputs[index];
      ASSERT_TRUE(builder.store_bits_bool(output.dst));
      ASSERT_TRUE(builder.store_ulong_rchk_bool(output.amount, 64));
      ASSERT_TRUE(builder.store_ulong_rchk_bool(output.fee, 64));
    }
    return builder.finalize();
  };
  vm::CellBuilder alternate_tree_builder;
  ASSERT_TRUE(alternate_tree_builder.store_ulong_rchk_bool(block::NativeTransferRun::outputs_node_magic, 32));
  ASSERT_TRUE(alternate_tree_builder.store_ulong_rchk_bool(1, 5));
  ASSERT_TRUE(alternate_tree_builder.store_ref_bool(make_output_leaf(0, 1)));
  ASSERT_TRUE(alternate_tree_builder.store_ref_bool(make_output_leaf(1, 2)));
  vm::CellBuilder valid_signature_builder;
  ASSERT_TRUE(valid_signature_builder.store_bytes_bool(three_run.signature));
  vm::CellBuilder alternate_root_builder;
  ASSERT_TRUE(alternate_root_builder.store_ulong_rchk_bool(block::NativeTransferRun::magic, 32));
  ASSERT_TRUE(alternate_root_builder.store_bits_bool(three_run.src));
  ASSERT_TRUE(alternate_root_builder.store_ulong_rchk_bool(three_run.first_nonce, 64));
  ASSERT_TRUE(alternate_root_builder.store_ulong_rchk_bool(three_run.valid_until, 32));
  ASSERT_TRUE(alternate_root_builder.store_ulong_rchk_bool(three_run.outputs.size(), 5));
  ASSERT_TRUE(alternate_root_builder.store_ref_bool(valid_signature_builder.finalize()));
  ASSERT_TRUE(alternate_root_builder.store_ref_bool(alternate_tree_builder.finalize()));
  ASSERT_TRUE(block::NativeTransferRun::unpack_external(alternate_root_builder.finalize()).is_error());
}

TEST(NativeStateEngine, native_transfer_batch_v5_keeps_signed_runs_atomic_and_flattens_entries) {
  auto source_key = td::Ed25519::generate_private_key().move_as_ok();
  auto source_public = source_key.get_public_key().move_as_ok().as_octet_string();
  ton::Bits256 domain;
  domain.as_slice().copy_from(std::string(32, '\x51'));

  auto make_run = [&](td::uint64 first_nonce, std::initializer_list<std::pair<td::uint64, td::uint64>> values,
                      unsigned destination_seed) {
    block::NativeTransferRun run;
    run.src.as_slice().copy_from(source_public);
    run.first_nonce = first_nonce;
    run.valid_until = std::numeric_limits<ton::UnixTime>::max();
    unsigned index = 0;
    for (const auto& [amount, fee] : values) {
      block::NativeTransferRunOutput output;
      std::string destination(32, '\0');
      destination[0] = 3;
      destination.back() = static_cast<char>(destination_seed + index++);
      output.dst.as_slice().copy_from(destination);
      output.amount = amount;
      output.fee = fee;
      run.outputs.push_back(std::move(output));
    }
    run.signature = source_key.sign(run.signing_payload(domain)).move_as_ok().as_slice().str();
    ASSERT_TRUE(run.verify_signature(domain).is_ok());
    return run;
  };

  block::NativeTransferBatch batch;
  batch.version = block::NativeTransferBatch::runs_version;
  batch.runs.push_back(make_run(40, {{7, 1}, {11, 2}}, 10));
  batch.runs.push_back(make_run(42, {{13, 3}}, 20));

  vm::CellBuilder builder;
  ASSERT_TRUE(batch.store(builder));
  auto root = builder.finalize();

  // The v5 transfer tree refers to the source-signed run cells directly. Its
  // branch weights are logical transfer counts, not run counts.
  auto header = vm::load_cell_slice(root);
  ASSERT_EQ(header.fetch_ulong(32), block::NativeTransferBatch::magic);
  ASSERT_EQ(header.fetch_ulong(8), block::NativeTransferBatch::runs_version);
  auto accounts_count = header.fetch_ulong(32);
  ASSERT_EQ(header.fetch_ulong(32), 3);
  td::Ref<vm::Cell> account_root, run_tree;
  ASSERT_TRUE(header.fetch_maybe_ref(account_root));
  ASSERT_TRUE(header.fetch_maybe_ref(run_tree));
  ASSERT_TRUE(header.empty_ext());
  ASSERT_EQ(accounts_count, 0);
  ASSERT_TRUE(account_root.is_null());
  auto run_node = vm::load_cell_slice(run_tree);
  ASSERT_EQ(run_node.fetch_ulong(32), block::NativeTransferBatch::runs_node_magic);
  ASSERT_EQ(run_node.fetch_ulong(32), 2);
  td::Ref<vm::Cell> left_run, right_run;
  ASSERT_TRUE(run_node.fetch_ref_to(left_run));
  ASSERT_TRUE(run_node.fetch_ref_to(right_run));
  ASSERT_TRUE(run_node.empty_ext());
  ASSERT_EQ(vm::load_cell_slice(left_run).prefetch_ulong(32), block::NativeTransferRun::magic);
  ASSERT_EQ(vm::load_cell_slice(right_run).prefetch_ulong(32), block::NativeTransferRun::magic);

  auto decoded = block::NativeTransferBatch::unpack(root).move_as_ok();
  ASSERT_EQ(decoded.version, block::NativeTransferBatch::runs_version);
  ASSERT_EQ(decoded.runs.size(), 2u);
  ASSERT_EQ(decoded.accounts.size(), 4u);
  ASSERT_EQ(decoded.entries.size(), 3u);
  ASSERT_EQ(decoded.entries[0].transfer.nonce, 40u);
  ASSERT_EQ(decoded.entries[1].transfer.nonce, 41u);
  ASSERT_EQ(decoded.entries[2].transfer.nonce, 42u);
  ASSERT_EQ(decoded.entries[0].transfer.signature, decoded.runs[0].signature);
  ASSERT_TRUE(decoded.runs[0].verify_signature(domain).is_ok());
  ASSERT_TRUE(decoded.runs[1].verify_signature(domain).is_ok());
  // A flattened v5 entry keeps the run's signature bytes only as a derived
  // execution view. It is intentionally not an individually signed NTFX.
  ASSERT_TRUE(decoded.entries[0].transfer.verify_signature(domain).is_error());

  // Validators replay the derived entries in this exact run/output order
  // after verifying the enclosing run signatures once. Exercise the state
  // engine with verification deliberately disabled: an individual flattened
  // entry must not be reinterpreted as a separately signed NTFX payload.
  td::uint64 source_balance = 1'000;
  td::uint64 source_nonce = 40;
  std::map<ton::StdSmcAddress, td::uint64> destination_balances;
  for (const auto& entry : decoded.entries) {
    auto& destination_balance = destination_balances[entry.transfer.dst];
    block::NativeTransferStateInput input{
        .transfer = &entry.transfer,
        .src_balance = source_balance,
        .src_nonce = source_nonce,
        .src_status = block::Account::acc_uninit,
        .src_is_native = true,
        .dst_balance = destination_balance,
        .dst_status = block::Account::acc_nonexist,
        .dst_is_native = false,
    };
    auto result = block::execute_native_transfer_state(input, decoded.entries.front().transfer.valid_until - 1,
                                                        /*verify_signature=*/false);
    ASSERT_EQ(result.code, block::NativeTransferStateResult::ok);
    source_balance = result.src_balance;
    source_nonce = result.src_nonce;
    destination_balance = result.dst_balance;
  }
  ASSERT_EQ(source_nonce, 43u);
  ASSERT_EQ(source_balance, 963u);
  ASSERT_EQ(destination_balances[decoded.entries[0].transfer.dst], 7u);
  ASSERT_EQ(destination_balances[decoded.entries[1].transfer.dst], 11u);
  ASSERT_EQ(destination_balances[decoded.entries[2].transfer.dst], 13u);

  // A single run is the transfer root itself; no synthetic leaf wrapper is
  // introduced around the signed external cell.
  block::NativeTransferBatch single_run_batch;
  single_run_batch.version = block::NativeTransferBatch::runs_version;
  single_run_batch.runs.push_back(batch.runs.front());
  vm::CellBuilder single_run_builder;
  ASSERT_TRUE(single_run_batch.store(single_run_builder));
  auto single_run_decoded = block::NativeTransferBatch::unpack(single_run_builder.finalize()).move_as_ok();
  ASSERT_EQ(single_run_decoded.runs.size(), 1u);
  ASSERT_EQ(single_run_decoded.entries.size(), 2u);

  vm::CellBuilder roundtrip_builder;
  ASSERT_TRUE(decoded.store(roundtrip_builder));
  auto roundtrip_root = roundtrip_builder.finalize();
  ASSERT_EQ(root->get_hash(), roundtrip_root->get_hash());

  auto inconsistent = decoded;
  ++inconsistent.entries[1].transfer.amount;
  vm::CellBuilder inconsistent_builder;
  ASSERT_TRUE(!inconsistent.store(inconsistent_builder));

  // The header contains the logical count. A direct run leaf cannot claim a
  // different number of output transfers, otherwise a run could be silently
  // split or truncated by a malformed batch tree.
  vm::CellBuilder first_run_builder;
  ASSERT_TRUE(batch.runs.front().store_external(first_run_builder));
  auto first_run_root = first_run_builder.finalize();
  vm::CellBuilder malformed_builder;
  ASSERT_TRUE(malformed_builder.store_ulong_rchk_bool(block::NativeTransferBatch::magic, 32));
  ASSERT_TRUE(malformed_builder.store_ulong_rchk_bool(block::NativeTransferBatch::runs_version, 8));
  ASSERT_TRUE(malformed_builder.store_ulong_rchk_bool(accounts_count, 32));
  ASSERT_TRUE(malformed_builder.store_ulong_rchk_bool(3, 32));
  ASSERT_TRUE(malformed_builder.store_maybe_ref(account_root));
  ASSERT_TRUE(malformed_builder.store_maybe_ref(first_run_root));
  ASSERT_TRUE(block::NativeTransferBatch::unpack(malformed_builder.finalize()).is_error());

  // Run leaves are canonical individually, but the enclosing vector must be
  // canonical as well.  Three one-output runs normally serialize as
  // [[first, second], third]; reject the equivalent [first, [second, third]].
  auto third_run = make_run(43, {{17, 4}}, 30);
  auto serialize_run = [](const block::NativeTransferRun& run) {
    vm::CellBuilder run_builder;
    ASSERT_TRUE(run.store_external(run_builder));
    return run_builder.finalize();
  };
  vm::CellBuilder right_branch_builder;
  ASSERT_TRUE(right_branch_builder.store_ulong_rchk_bool(block::NativeTransferBatch::runs_node_magic, 32));
  ASSERT_TRUE(right_branch_builder.store_ulong_rchk_bool(1, 32));
  ASSERT_TRUE(right_branch_builder.store_ref_bool(serialize_run(batch.runs[1])));
  ASSERT_TRUE(right_branch_builder.store_ref_bool(serialize_run(third_run)));
  auto right_branch = right_branch_builder.finalize();
  vm::CellBuilder alternate_root_builder;
  ASSERT_TRUE(alternate_root_builder.store_ulong_rchk_bool(block::NativeTransferBatch::runs_node_magic, 32));
  ASSERT_TRUE(alternate_root_builder.store_ulong_rchk_bool(2, 32));
  ASSERT_TRUE(alternate_root_builder.store_ref_bool(serialize_run(batch.runs[0])));
  ASSERT_TRUE(alternate_root_builder.store_ref_bool(right_branch));
  auto alternate_root = alternate_root_builder.finalize();
  vm::CellBuilder alternate_batch_builder;
  ASSERT_TRUE(alternate_batch_builder.store_ulong_rchk_bool(block::NativeTransferBatch::magic, 32));
  ASSERT_TRUE(alternate_batch_builder.store_ulong_rchk_bool(block::NativeTransferBatch::runs_version, 8));
  ASSERT_TRUE(alternate_batch_builder.store_ulong_rchk_bool(0, 32));
  ASSERT_TRUE(alternate_batch_builder.store_ulong_rchk_bool(4, 32));
  ASSERT_TRUE(alternate_batch_builder.store_maybe_ref(td::Ref<vm::Cell>{}));
  ASSERT_TRUE(alternate_batch_builder.store_maybe_ref(alternate_root));
  ASSERT_TRUE(block::NativeTransferBatch::unpack(alternate_batch_builder.finalize()).is_error());
}

TEST(NativeStateEngine, compact_batch_v3_balanced_tree) {
  block::NativeTransferBatch batch;
  batch.version = 3;
  constexpr std::size_t transfer_count = 5000;
  batch.entries.reserve(transfer_count);
  for (std::size_t i = 0; i < transfer_count; ++i) {
    block::NativeTransfer transfer;
    std::string source(32, '\0'), destination(32, '\0');
    source[0] = 2;
    destination[0] = 3;
    for (std::size_t byte = 0; byte < sizeof(i); ++byte) {
      source[31 - byte] = static_cast<char>(i >> (byte * 8));
      destination[31 - byte] = static_cast<char>(i >> (byte * 8));
    }
    transfer.src.as_slice().copy_from(source);
    transfer.dst.as_slice().copy_from(destination);
    transfer.amount = i + 1;
    transfer.nonce = i;
    transfer.valid_until = std::numeric_limits<ton::UnixTime>::max();
    transfer.signature.assign(64, static_cast<char>(i));
    ASSERT_TRUE(transfer.is_valid());
    batch.entries.push_back({std::move(transfer), 0, 0});
  }

  vm::CellBuilder builder;
  ASSERT_TRUE(batch.store(builder));
  auto root = builder.finalize();
  ASSERT_TRUE(root->get_depth() < 64);
  auto unpacked = block::NativeTransferBatch::unpack(root).move_as_ok();
  ASSERT_EQ(unpacked.version, 3);
  ASSERT_EQ(unpacked.accounts.size(), transfer_count * 2);
  ASSERT_EQ(unpacked.entries.size(), transfer_count);
  ASSERT_EQ(unpacked.entries.front().transfer.src, batch.entries.front().transfer.src);
  ASSERT_EQ(unpacked.entries.back().transfer.dst, batch.entries.back().transfer.dst);
  ASSERT_EQ(unpacked.entries.back().transfer.nonce, transfer_count - 1);
}

TEST(NativeStateEngine, compact_batch_v4_balanced_tree) {
  block::NativeTransferBatch batch;
  ASSERT_EQ(batch.version, block::NativeTransferBatch::current_version);
  constexpr std::size_t transfer_count = 8192;
  batch.entries.reserve(transfer_count);
  for (std::size_t i = 0; i < transfer_count; ++i) {
    block::NativeTransfer transfer;
    std::string source(32, '\0'), destination(32, '\0');
    source[0] = 2;
    destination[0] = 3;
    for (std::size_t byte = 0; byte < sizeof(i); ++byte) {
      source[31 - byte] = static_cast<char>(i >> (byte * 8));
    }
    transfer.src.as_slice().copy_from(source);
    transfer.dst.as_slice().copy_from(destination);
    transfer.amount = 1;
    transfer.nonce = 0;
    transfer.valid_until = std::numeric_limits<ton::UnixTime>::max();
    transfer.signature.assign(64, static_cast<char>(i));
    ASSERT_TRUE(transfer.is_valid());
    batch.entries.push_back({std::move(transfer), 0, 0});
  }

  vm::CellBuilder builder;
  ASSERT_TRUE(batch.store(builder));
  auto root = builder.finalize();
  ASSERT_TRUE(root->get_depth() < 64);
  auto unpacked = block::NativeTransferBatch::unpack(root).move_as_ok();
  ASSERT_EQ(unpacked.version, 4);
  ASSERT_EQ(unpacked.accounts.size(), transfer_count + 1);
  ASSERT_EQ(unpacked.entries.size(), transfer_count);
  ASSERT_EQ(unpacked.entries.front().transfer.dst, unpacked.entries.back().transfer.dst);
}

TEST(NativeStateEngine, compact_encoding_sizes) {
  auto make_transfer = [](std::size_t i, bool shared_destination) {
    block::NativeTransfer transfer;
    std::string source(32, '\0'), destination(32, '\0');
    source[0] = 2;
    destination[0] = 3;
    for (std::size_t byte = 0; byte < sizeof(i); ++byte) {
      source[31 - byte] = static_cast<char>(i >> (byte * 8));
      if (!shared_destination) {
        destination[31 - byte] = static_cast<char>(i >> (byte * 8));
      }
    }
    transfer.src.as_slice().copy_from(source);
    transfer.dst.as_slice().copy_from(destination);
    transfer.amount = 1;
    transfer.nonce = i;
    transfer.valid_until = std::numeric_limits<ton::UnixTime>::max();
    transfer.signature.assign(64, static_cast<char>(i));
    return transfer;
  };

  auto external = make_transfer(0, false);
  vm::CellBuilder external_builder;
  ASSERT_TRUE(external.store_external(external_builder));
  auto external_root = external_builder.finalize();
  vm::NewCellStorageStat external_stat;
  external_stat.add_cell(external_root);
  auto external_boc = vm::std_boc_serialize(external_root).move_as_ok();
  ASSERT_EQ(external_stat.get_stat().cells, 2u);
  ASSERT_EQ(external_stat.get_stat().bits, 1280u);
  ASSERT_EQ(external_boc.size(), 176u);

  vm::CellBuilder native_state_builder;
  ASSERT_TRUE(native_state_builder.store_long_bool(1, 2));
  ASSERT_TRUE(native_state_builder.store_ulong_rchk_bool(1, 64));
  ASSERT_TRUE(native_state_builder.store_ulong_rchk_bool(0, 64));
  ASSERT_TRUE(native_state_builder.store_ulong_rchk_bool(0, 8));
  auto native_state = native_state_builder.finalize();
  vm::CellBuilder shard_account_builder;
  ASSERT_TRUE(shard_account_builder.store_ref_bool(native_state));
  ASSERT_TRUE(shard_account_builder.store_bits_bool(ton::Bits256{}));
  ASSERT_TRUE(shard_account_builder.store_ulong_rchk_bool(0, 64));
  auto shard_account = shard_account_builder.finalize();
  vm::NewCellStorageStat shard_account_stat;
  shard_account_stat.add_cell(shard_account);
  ASSERT_EQ(shard_account_stat.get_stat().cells, 2u);
  ASSERT_EQ(shard_account_stat.get_stat().bits, 458u);
  ASSERT_EQ(vm::std_boc_serialize(native_state).move_as_ok().size(), 31u);
  ASSERT_EQ(vm::std_boc_serialize(shard_account).move_as_ok().size(), 74u);

  // Lock down the compact v4 wire cost at one collator microbatch. With
  // unique endpoints, the account table contributes materially more than in
  // the shared-destination load shape even though transfer leaves are fixed.
  constexpr std::size_t count = 512;
  for (bool shared_destination : {false, true}) {
    block::NativeTransferBatch batch;
    batch.entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      batch.entries.push_back({make_transfer(i, shared_destination), 0, 0});
    }
    vm::CellBuilder builder;
    ASSERT_TRUE(batch.store(builder));
    auto root = builder.finalize();
    vm::NewCellStorageStat stat;
    stat.add_cell(root);
    auto boc = vm::std_boc_serialize(root).move_as_ok();
    auto accounts = block::NativeTransferBatch::unpack(root).move_as_ok().accounts.size();
    if (shared_destination) {
      ASSERT_EQ(accounts, 513u);
      ASSERT_EQ(stat.get_stat().cells, 1365u);
      ASSERT_EQ(stat.get_stat().bits, 607842u);
      ASSERT_EQ(boc.size(), 81456u);
    } else {
      ASSERT_EQ(accounts, 1024u);
      ASSERT_EQ(stat.get_stat().cells, 1707u);
      ASSERT_EQ(stat.get_stat().bits, 756442u);
      ASSERT_EQ(boc.size(), 101399u);
    }
  }
}

TEST(NativeStateEngine, compact_batch_rejects_unbounded_header_counts) {
  auto dummy_ref = [] {
    vm::CellBuilder builder;
    ASSERT_TRUE(builder.store_ulong_rchk_bool(0, 1));
    return builder.finalize();
  };
  auto make_header = [&](td::uint32 accounts_count, td::uint32 entries_count, bool accounts_root,
                         bool entries_root) {
    vm::CellBuilder builder;
    ASSERT_TRUE(builder.store_ulong_rchk_bool(block::NativeTransferBatch::magic, 32));
    ASSERT_TRUE(builder.store_ulong_rchk_bool(block::NativeTransferBatch::current_version, 8));
    ASSERT_TRUE(builder.store_ulong_rchk_bool(accounts_count, 32));
    ASSERT_TRUE(builder.store_ulong_rchk_bool(entries_count, 32));
    ASSERT_TRUE(builder.store_maybe_ref(accounts_root ? dummy_ref() : td::Ref<vm::Cell>{}));
    ASSERT_TRUE(builder.store_maybe_ref(entries_root ? dummy_ref() : td::Ref<vm::Cell>{}));
    return builder.finalize();
  };

  ASSERT_TRUE(block::NativeTransferBatch::unpack(
                  make_header(block::NativeTransferBatch::max_accounts + 1, 1, true, true))
                  .is_error());
  ASSERT_TRUE(block::NativeTransferBatch::unpack(
                  make_header(0, block::NativeTransferBatch::max_entries + 1, false, true))
                  .is_error());
  ASSERT_TRUE(block::NativeTransferBatch::unpack(make_header(3, 1, true, true)).is_error());
}

TEST(NativeStateEngine, compact_batch_version_and_run_capability_activation) {
  ASSERT_EQ(ton::SUPPORTED_VERSION, block::NativeTransferBatch::runs_global_version);
  ASSERT_TRUE(block::NativeTransferBatch::version_allowed_for_global_version(3, 13));
  ASSERT_TRUE(block::NativeTransferBatch::version_allowed_for_global_version(4, 13));
  ASSERT_TRUE(!block::NativeTransferBatch::version_allowed_for_global_version(
      block::NativeTransferBatch::runs_version, 13));
  ASSERT_TRUE(!block::NativeTransferBatch::version_allowed_for_global_version(3, 14));
  ASSERT_TRUE(block::NativeTransferBatch::version_allowed_for_global_version(4, 14));
  ASSERT_TRUE(!block::NativeTransferBatch::version_allowed_for_global_version(
      block::NativeTransferBatch::runs_version, 14));

  // Legacy callers intentionally retain the two-argument gate. The
  // collator/validator use the capability-aware gate below for v5, so an
  // accidental call site cannot enable a run batch by version alone.
  ASSERT_TRUE(!block::NativeTransferBatch::version_allowed_for_global_version(
      block::NativeTransferBatch::runs_version, block::NativeTransferBatch::runs_global_version));

  // The future opt-in is deliberately two-dimensional: neither a version
  // bump alone nor an advertised capability alone can enable NTRN runs.
  ASSERT_TRUE(!block::NativeTransferBatch::version_allowed_for_global_version_and_capabilities(
      block::NativeTransferBatch::runs_version, block::NativeTransferBatch::runs_global_version - 1,
      block::NativeTransferBatch::runs_capability));
  ASSERT_TRUE(!block::NativeTransferBatch::version_allowed_for_global_version_and_capabilities(
      block::NativeTransferBatch::runs_version, block::NativeTransferBatch::runs_global_version, 0));
  ASSERT_TRUE(block::NativeTransferBatch::version_allowed_for_global_version_and_capabilities(
      block::NativeTransferBatch::runs_version, block::NativeTransferBatch::runs_global_version,
      block::NativeTransferBatch::runs_capability));

  // Scalar batches remain available before the v15 run-capability switch,
  // including if the capability bit is configured early. Once both v15 gates
  // are enabled, the fresh chain has one authorization model only.
  ASSERT_TRUE(block::NativeTransferBatch::version_allowed_for_global_version_and_capabilities(
      4, block::NativeTransferBatch::runs_global_version - 1, block::NativeTransferBatch::runs_capability));
  ASSERT_TRUE(block::NativeTransferBatch::version_allowed_for_global_version_and_capabilities(4, 15, 0));
  ASSERT_TRUE(!block::NativeTransferBatch::version_allowed_for_global_version_and_capabilities(
      4, 15, ton::capNativeTransferRuns));
  ASSERT_TRUE(!block::NativeTransferBatch::version_allowed_for_global_version_and_capabilities(3, 15,
                                                                                                  ton::capNativeTransferRuns));
}
