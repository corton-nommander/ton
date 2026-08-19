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
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "common/bigexp.h"
#include "common/bigint.hpp"
#include "common/bitstring.h"
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

TEST(NativeStateEngine, compact_batch_domain_signature_activation_rejects_downgrade) {
  ASSERT_TRUE(block::NativeTransferBatch::version_allowed_for_global_version(3, 13));
  ASSERT_TRUE(block::NativeTransferBatch::version_allowed_for_global_version(4, 13));
  ASSERT_TRUE(!block::NativeTransferBatch::version_allowed_for_global_version(3, 14));
  ASSERT_TRUE(block::NativeTransferBatch::version_allowed_for_global_version(4, 14));
}
