
#include "test_common.hpp"
#include <acl/allocators/default_allocator.hpp>
#include <acl/allocators/std_allocator_wrapper.hpp>
#include <acl/containers/index_map.hpp>
#include <acl/containers/packed_table.hpp>
#include <acl/containers/sparse_table.hpp>
#include <acl/utils/error_codes.hpp>
#include <acl/utils/export.hxx>
#include <acl/utils/intrusive_ptr.hpp>
#include <acl/utils/komihash.hpp>
#include <acl/utils/link.hpp>
#include <acl/utils/tagged_ptr.hpp>
#include <acl/utils/wyhash.hpp>
#include <acl/utils/zip_view.hpp>
#include <catch2/catch_all.hpp>
#include <span>

#define BINARY_SEARCH_STEP                                                                                             \
  do                                                                                                                   \
  {                                                                                                                    \
    const size_t* const middle = it + (size >> 1);                                                                     \
    size                       = (size + 1) >> 1;                                                                      \
    it                         = *middle < key ? middle : it;                                                          \
  }                                                                                                                    \
  while (0)

static inline auto mini0(size_t const* it, size_t size, size_t key) noexcept
{
  while (size > 2)
    BINARY_SEARCH_STEP;
  it += size > 1 && (*it < key);
  it += size > 0 && (*it < key);
  return it;
}

static inline auto mini1(size_t const* it, size_t size, size_t key) noexcept
{
  do
  {
    BINARY_SEARCH_STEP;
  }
  while (size > 2);
  it += size > 1 && (*it < key);
  it += size > 0 && (*it < key);
  return it;
}

static inline auto mini2(size_t const* it, size_t size, size_t key) noexcept
{
  do
  {
    BINARY_SEARCH_STEP;
    BINARY_SEARCH_STEP;
  }
  while (size > 2);
  it += size > 1 && (*it < key);
  it += size > 0 && (*it < key);
  return it;
}

TEST_CASE("Lower bound")
{
  std::vector<size_t> vec{3, 20, 60, 400};

  auto i = mini0(vec.data(), 3, 40);
  REQUIRE(i < vec.data() + vec.size());
}

TEST_CASE("Validate malloc")
{
  char* data = (char*)acl::detail::malloc(100);
  std::memset(data, 1, 100);
  acl::detail::free(data);

  data = (char*)acl::detail::zmalloc(100);
  for (int i = 0; i < 100; ++i)
    REQUIRE(data[i] == 0);
  acl::detail::free(data);

  data = (char*)acl::detail::aligned_alloc(16, 16);
  REQUIRE((reinterpret_cast<uintptr_t>(data) & 15) == 0);
  acl::detail::aligned_free(data);

  data = (char*)acl::detail::aligned_zmalloc(16, 16);
  REQUIRE((reinterpret_cast<uintptr_t>(data) & 15) == 0);
  for (int i = 0; i < 16; ++i)
    REQUIRE(data[i] == 0);
  acl::detail::aligned_free(data);
}

struct myBase
{
  int& c;
  myBase(int& a) : c(a) {}
  virtual ~myBase() = default;
};

struct myClass : myBase
{
  myClass(int& a) : myBase(a) {}
  ~myClass()
  {
    c = -1;
  }
};

int intrusive_count_add(myBase* m)
{
  return m->c++;
}
int intrusive_count_sub(myBase* m)
{
  return m->c--;
}
int intrusive_count_get(myBase* m)
{
  return m->c;
}

TEST_CASE("Validate smart pointer", "[intrusive_ptr]")
{
  int check = 0;

  acl::intrusive_ptr<myClass> ptr;
  CHECK(ptr == nullptr);
  ptr = acl::intrusive_ptr<myClass>(new myClass(check));
  CHECK(ptr != nullptr);
  CHECK(ptr.use_count() == 1);
  auto copy = ptr;
  CHECK(ptr.use_count() == 2);
  CHECK(ptr->c == 2);
  ptr.reset();
  CHECK(check == 1);
  ptr = std::move(copy);
  CHECK(ptr.use_count() == 1);
  // Static cast
  acl::intrusive_ptr<myBase> base  = acl::static_pointer_cast<myBase>(ptr);
  acl::intrusive_ptr<myBase> base2 = ptr;
  CHECK(ptr.use_count() == 2);

  base.reset();
  ptr.reset();

  CHECK(check == -1);
}

TEST_CASE("Validate general_allocator", "[general_allocator]")
{

  using namespace acl;
  using allocator_t   = default_allocator<acl::options<acl::opt::compute_stats>>;
  using std_allocator = allocator_wrapper<int, allocator_t>;
  [[maybe_unused]] std_allocator allocator;

  allocator_t::address data = allocator_t::allocate(256, 128);
  CHECK((reinterpret_cast<std::uintptr_t>(data) & 127) == 0);
  allocator_t::deallocate(data, 256, 128);
  // Should be fine to free nullptr
  allocator_t::deallocate(nullptr, 0, 0);
}

TEST_CASE("Validate tagged_ptr", "[tagged_ptr]")
{
  using namespace acl;
  detail::tagged_ptr<std::string> tagged_string;

  std::string my_string = "This is my string";
  std::string copy      = my_string;

  tagged_string.set(&my_string, 1);

  CHECK(tagged_string.get_ptr() == &my_string);
  CHECK(*tagged_string.get_ptr() == copy);

  tagged_string.set(&my_string, tagged_string.get_next_tag());

  CHECK(tagged_string.get_tag() == 2);

  CHECK(tagged_string.get_ptr() == &my_string);
  CHECK(*tagged_string.get_ptr() == copy);

  auto second = detail::tagged_ptr(&my_string, 2);
  CHECK((tagged_string == second) == true);

  tagged_string.set(&my_string, tagged_string.get_next_tag());
  CHECK(tagged_string != second);

  detail::tagged_ptr<std::void_t<>> null = nullptr;
  CHECK(!null);
}

TEST_CASE("Validate compressed_ptr", "[compressed_ptr]")
{
  using namespace acl;
  detail::compressed_ptr<std::string> tagged_string;

  std::string my_string = "This is my string";
  std::string copy      = my_string;

  tagged_string.set(&my_string, 1);

  CHECK(tagged_string.get_ptr() == &my_string);
  CHECK(*tagged_string.get_ptr() == copy);

  tagged_string.set(&my_string, tagged_string.get_next_tag());

  CHECK(tagged_string.get_tag() == 2);

  CHECK(tagged_string.get_ptr() == &my_string);
  CHECK(*tagged_string.get_ptr() == copy);

  auto second = detail::compressed_ptr(&my_string, 2);
  CHECK((tagged_string == second) == true);

  tagged_string.set(&my_string, tagged_string.get_next_tag());
  CHECK(tagged_string != second);

  detail::compressed_ptr<std::void_t<>> null = nullptr;
  CHECK(!null);
}

TEST_CASE("Validate error_codes", "[error_code]")
{
  auto ec = acl::make_error_code(acl::serializer_error::corrupt_array_item);

  CHECK(&ec.category() == &acl::error_category<acl::serializer_error>::instance());
  CHECK(!ec.message().empty());
  CHECK(ec.category().name() != nullptr);
}

TEST_CASE("Validate Hash: wyhash", "[hash]")
{
  constexpr std::string_view cs = "A long string whose hash we are about to find out !";
  std::string                s{cs};
  acl::wyhash32              wyh32;

  auto value32 = wyh32(s.c_str(), s.length());
  REQUIRE(value32 != 0);

  auto new_value32 = wyh32(s.c_str(), s.length());
  REQUIRE(value32 != new_value32);

  constexpr auto constval = acl::wyhash32::make(cs.data(), cs.size());
  REQUIRE(value32 == constval);

  REQUIRE(wyh32() == new_value32);

  acl::wyhash64 wyh64;

  auto value64 = wyh64(s.c_str(), s.length());
  REQUIRE(value64 != 0);

  auto new_value64 = wyh64(s.c_str(), s.length());
  REQUIRE(value64 != new_value64);

  REQUIRE(wyh64() == new_value64);
}

TEST_CASE("Validate Hash: komihash", "[hash]")
{
  std::string     s = "A long string whose hash we are about to find out !";
  acl::komihash64 k64;

  auto value = k64(s.c_str(), s.length());
  REQUIRE(value != 0);

  auto new_value = k64(s.c_str(), s.length());
  REQUIRE(value != new_value);

  REQUIRE(k64() == new_value);

  acl::komihash64_stream k64s;

  k64s(s.c_str(), s.length());
  k64s(s.c_str(), s.length());

  REQUIRE(k64s() != 0);
}

TEST_CASE("Test link", "[link]")
{
  struct base
  {};
  struct derived : base
  {};
  struct unrelated
  {};

  acl::link<base, uint32_t, 4>      first;
  acl::link<derived, uint32_t, 4>   second;
  acl::link<unrelated, uint32_t, 4> third;

  second = first;
  first  = second;
  static_assert(std::is_assignable_v<acl::link<base, uint32_t, 4>, acl::link<derived, uint32_t, 4>>,
                "Type should be assignable");
  static_assert(std::is_assignable_v<acl::link<base, uint32_t, 4>, acl::link<derived, uint32_t, 4>>,
                "Type should be assignable");
  static_assert(!std::is_assignable_v<acl::link<unrelated, uint32_t, 4>, acl::link<base, uint32_t, 4>>,
                "Type should not be assignable");
  static_assert(!std::is_assignable_v<acl::link<unrelated, uint32_t, 4>, acl::link<derived, uint32_t, 4>>,
                "Type should not be assignable");
  static_assert(!std::is_assignable_v<acl::link<base, uint32_t, 4>, acl::link<unrelated, uint32_t, 4>>,
                "Type should not be assignable");
  static_assert(!std::is_assignable_v<acl::link<derived, uint32_t, 4>, acl::link<unrelated, uint32_t, 4>>,
                "Type should not be assignable");
}

TEST_CASE("Test index_map", "[index_map]")
{
  acl::index_map<uint32_t, 5> map;

  REQUIRE(map.empty() == true);
  map[24] = 5;
  REQUIRE(map.size() == 1);
  REQUIRE(map[24] == 5);
  map[25] = 25;
  REQUIRE(map[24] == 5);
  REQUIRE(map[25] == 25);
  REQUIRE(map.base_offset() == 24);
  map[15] = 15;
  REQUIRE(map[24] == 5);
  REQUIRE(map[25] == 25);
  REQUIRE(map[15] == 15);
  REQUIRE(map.size() == 11);
  REQUIRE(map.base_offset() == 15);
  map[40] = 40;
  REQUIRE(map[24] == 5);
  REQUIRE(map[25] == 25);
  REQUIRE(map[15] == 15);
  REQUIRE(map[40] == 40);
  REQUIRE(map.size() == 26);
  REQUIRE(map.base_offset() == 15);
  map[31] = 31;
  REQUIRE(map[24] == 5);
  REQUIRE(map[25] == 25);
  REQUIRE(map[15] == 15);
  REQUIRE(map[40] == 40);
  REQUIRE(map[31] == 31);
  REQUIRE(map.base_offset() == 15);
  map[41] = 41;
  REQUIRE(map[24] == 5);
  REQUIRE(map[25] == 25);
  REQUIRE(map[15] == 15);
  REQUIRE(map[40] == 40);
  REQUIRE(map[31] == 31);
  REQUIRE(map[41] == 41);
  REQUIRE(map.base_offset() == 15);
  map[2] = 2;
  REQUIRE(map[2] == 2);
  REQUIRE(map[15] == 15);
  REQUIRE(map[24] == 5);
  REQUIRE(map[25] == 25);
  REQUIRE(map[31] == 31);
  REQUIRE(map[40] == 40);
  REQUIRE(map[41] == 41);
  REQUIRE(map.base_offset() == 0);
  REQUIRE(map.empty() == false);

  auto     data = std::array{2, 15, 5, 25, 31, 40, 41};
  uint32_t it   = 0;
  for (auto& i : map)
  {
    if (i != map.null)
    {
      REQUIRE(i == data[it++]);
    }
  }

  for (auto r = map.rbegin(); r != map.rend(); ++r)
  {
    if (*r != map.null)
    {
      REQUIRE(*r == data[--it]);
    }
  }
}

TEST_CASE("Test index_map fuzz test", "[index_map]")
{
  acl::index_map<uint32_t, 64> map;
  std::vector<uint32_t>        full_map;

  uint32_t fixed_seed = Catch::rngSeed();

  auto seed = xorshift32(fixed_seed);
  auto end  = seed % 100;
  full_map.resize(100, 99999);
  for (uint32_t i = 0; i < end; ++i)
  {
    auto idx      = (seed = xorshift32(seed)) % 100;
    full_map[idx] = map[idx] = ((seed = xorshift32(seed)) % 1000);
  }

  for (uint32_t i = 0; i < end; ++i)
  {
    if (full_map[i] == 99999)
      REQUIRE(map[i] == map.null);
    else
      REQUIRE(map[i] == full_map[i]);
  }
}

TEST_CASE("Test zip_view", "[zip_view]")
{
  std::vector<std::string> strings;
  std::vector<int32_t>     integers;

  for (int i = 0; i < 10; ++i)
  {
    strings.emplace_back(std::to_string(i) + "-item");
    integers.emplace_back(i * 10);
  }
  int start = 0;
  for (auto&& [val, ints] : acl::zip(strings, integers))
  {
    REQUIRE(val == std::to_string(start) + "-item");
    REQUIRE(ints == start * 10);
    start++;
  }

  start = 0;
  for (auto&& [val, ints] : acl::zip(std::span(strings), std::span(integers)))
  {
    REQUIRE(val == std::to_string(start) + "-item");
    REQUIRE(ints == start * 10);
    start++;
  }
}
