#define BOOST_TEST_MODULE ext2dBox

#ifdef HAS_BOOST_UNIT_TEST_LIBRARY
// Shared library version
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>
#else
// Header only version
#include <boost/test/included/unit_test.hpp>
#endif

#include "rcx/ext2dBox.h"

namespace rcx {

BOOST_AUTO_TEST_CASE(simple_instantiate_accessors)
{
  ext2dBox box({0, 1}, {2, 4}, 1, 0, 0, /*dir=*/false);

  BOOST_TEST(box.dir() == false);
  BOOST_TEST(box.ll0() == 0);
  BOOST_TEST(box.ll1() == 1);
  BOOST_TEST(box.ur0() == 2);
  BOOST_TEST(box.ur1() == 4);

  BOOST_TEST(box.length() == 2);
  BOOST_TEST(box.width() == 3);
}

BOOST_AUTO_TEST_CASE(simple_rotate)
{
  ext2dBox box(/*ll=*/{0, 1}, /*ur=*/{2, 4}, 1, 0, 0, /*dir=*/false);

  box.rotate();

  BOOST_TEST(box.dir() == true);
  BOOST_TEST(box.ll0() == 1);
  BOOST_TEST(box.ll1() == 0);
  BOOST_TEST(box.ur0() == 4);
  BOOST_TEST(box.ur1() == 2);

  BOOST_TEST(box.length() == 2);
  BOOST_TEST(box.width() == 3);
}

BOOST_AUTO_TEST_CASE(simple_print_geoms_3d)
{
  ext2dBox box(/*ll=*/{0, 1}, /*ur=*/{2, 4}, 1, 0, 0, /*dir=*/false);

  FILE* tmp = tmpfile();
  const std::array<int, 2> orig = {0, 0};
  box.printGeoms3D(tmp, .5, .25, orig);
  BOOST_TEST(fseek(tmp, 0, SEEK_SET) == 0);

  constexpr size_t kBufSize = 1024;
  std::array<uint8_t, kBufSize> buf = {0};
  size_t total_read = 0;
  while (!feof(tmp)) {
    size_t read = fread(buf.data() + total_read, 1, kBufSize - total_read, tmp);
    total_read += read;
  }
  BOOST_TEST(total_read != 0);
  std::string_view s(reinterpret_cast<char*>(buf.data()));
  BOOST_TEST(s == "  0        0 -- M1 D0  0 0.001  0.002 0.004  L= 0.002 W= 0.003  H= 0.5  TH= 0.25 ORIG 0 0.001\n");
}

}  // namespace rcx
