
#pragma once
#include <acl/allocators/linear_arena_allocator.hpp>
#include <acl/dsl/yaml.hpp>
#include <acl/utils/error_codes.hpp>
#include <acl/utils/reflection.hpp>
#include <acl/utils/reflection_utils.hpp>
#include <acl/utils/type_traits.hpp>
#include <functional>

namespace acl
{
namespace detail
{
class parser_state;

struct in_context_base : public acl::yaml::context
{
  using pop_fn = void (*)(in_context_base*);

  parser_state&    parser_state_;
  in_context_base* parent_  = nullptr;
  pop_fn           pop_fn_  = nullptr;
  uint32_t         xvalue_  = 0;
  bool             is_null_ = false;

  in_context_base(parser_state& parser_state, in_context_base* parent) noexcept
      : parser_state_(parser_state), parent_(parent)
  {}
};

class parser_state
{
  yaml::istream                 stream;
  acl::linear_arena_allocator<> allocator;
  in_context_base*              context = nullptr;

public:
  parser_state(std::string_view content) noexcept : stream(content), allocator(8096) {}
  ~parser_state() noexcept
  {
    while (context)
    {
      auto parent = context->parent_;
      if (context->pop_fn_)
      {
        context->pop_fn_(context);
      }
      context = parent;
    }
  }

  template <typename C>
  void parse(C& handler)
  {
    stream.set_handler(&handler);
    handler.setup_proxy();
    stream.parse();
  }

  template <typename Context, typename... Args>
  inline Context* push(Args&&... args)
  {
    void* cursor   = allocator.allocate(sizeof(Context), alignof(Context));
    auto  icontext = std::construct_at(reinterpret_cast<Context*>(cursor), std::forward<Args>(args)...);
    stream.set_handler(icontext);
    icontext->setup_proxy();
    context = icontext;
    return icontext;
  }

  template <typename Context>
  inline void pop(Context* ptr, in_context_base* parent)
  {
    std::destroy_at(ptr);
    allocator.deallocate(ptr, sizeof(Context), alignof(Context));
    stream.set_handler(parent);
    context = parent;
  }
};

template <typename Class>
class icontext : public in_context_base
{
  using pop_fn     = std::function<void(void*)>;
  using class_type = detail::remove_cref<Class>;

public:
  icontext(class_type& obj, parser_state& state, in_context_base* parent) noexcept
    requires(std::is_reference_v<Class>)
      : obj_(obj), in_context_base(state, parent)
  {}

  icontext(parser_state& state, in_context_base* parent) noexcept
    requires(!std::is_reference_v<Class>)
      : in_context_base(state, parent)
  {}

  class_type& get() noexcept
  {
    return obj_;
  }

  void begin_key(std::string_view key) override
  {
    if constexpr (detail::BoundClass<class_type>)
    {
      if constexpr (detail::StringMapValueType<class_type>)
      {
        read_string_map_value(key);
      }
      else
      {
        read_bound_class(key);
      }
    }
    else if constexpr (detail::VariantLike<class_type>)
    {
      read_variant(key);
    }
  }

  void end_key() override
  {
    if (pop_fn_)
    {
      pop_fn_(this);
    }
  }

  void begin_array() override
  {
    if constexpr (detail::ContainerLike<class_type>)
    {
      push_container_item();
    }
    else if constexpr (detail::TupleLike<class_type>)
    {
      read_tuple();
    }
  }

  void end_array() override
  {
    if (pop_fn_)
    {
      pop_fn_(this);
    }
  }

  void setup_proxy()
  {
    if constexpr (detail::PointerLike<class_type>)
    {
      return read_pointer();
    }
    else if constexpr (detail::OptionalLike<class_type>)
    {
      return read_optional();
    }
  }

  void set_value(std::string_view slice) override
  {
    if (slice == "null")
    {
      is_null_ = true;
      return;
    }

    if constexpr (detail::TransformFromString<class_type>)
    {
      acl::from_string(obj_, slice);
    }
    else if constexpr (detail::ConstructedFromStringView<class_type>)
    {
      obj_ = class_type(slice);
    }
    else if constexpr (detail::ContainerIsStringLike<class_type>)
    {
      obj_ = class_type(slice);
    }
    else if constexpr (detail::BoolLike<class_type>)
      return read_bool(slice);
    else if constexpr (detail::IntegerLike<class_type>)
      return read_integer(slice);
    else if constexpr (detail::EnumLike<class_type>)
      return read_enum(slice);
    else if constexpr (detail::FloatLike<class_type>)
      return read_float(slice);
    else if constexpr (detail::MonostateLike<class_type>)
    {
    }
  }

  void read_bool(std::string_view slice)
  {
    using namespace std::string_view_literals;
    obj_ = slice == "true"sv || slice == "True"sv;
  }

  void error_check(std::from_chars_result result)
  {
    if (result.ec != std::errc())
    {
      throw std::runtime_error("Failed to parse value");
    }
  }

  void read_integer(std::string_view slice)
  {
    using namespace std::string_view_literals;
    if (slice.starts_with("0x"sv))
      error_check(std::from_chars(slice.data(), slice.data() + slice.size(), obj_, 16));
    else
      error_check(std::from_chars(slice.data(), slice.data() + slice.size(), obj_, 10));
  }

  void read_float(std::string_view slice)
  {
    using namespace std::string_view_literals;
    if (slice == ".nan"sv || slice == "nan"sv)
      obj_ = std::numeric_limits<class_type>::quiet_NaN();
    else if (slice == ".inf"sv || slice == "inf"sv)
      obj_ = std::numeric_limits<class_type>::infinity();
    else if (slice == "-.inf"sv || slice == "-inf"sv)
      obj_ = -std::numeric_limits<class_type>::infinity();
    else
      error_check(std::from_chars(slice.data(), slice.data() + slice.size(), obj_));
  }

  void read_enum(std::string_view slice)
  {
    std::underlying_type_t<class_type> value;
    if (slice.starts_with("0x"))
      error_check(std::from_chars(slice.data(), slice.data() + slice.size(), value, 16));
    else
      error_check(std::from_chars(slice.data(), slice.data() + slice.size(), value, 10));
    obj_ = static_cast<class_type>(value);
  }

  void read_pointer()
  {
    using class_type  = detail::remove_cref<class_type>;
    using pvalue_type = detail::pointer_class_type<class_type>;
    if (!obj_)
    {
      if constexpr (std::same_as<class_type, std::shared_ptr<pvalue_type>>)
        obj_ = std::make_shared<pvalue_type>();
      else
        obj_ = class_type(new detail::pointer_class_type<class_type>());
    }

    auto mapping     = parser_state_.push<icontext<pvalue_type&>>(*obj_, parser_state_, this);
    mapping->pop_fn_ = [](in_context_base* mapping)
    {
      auto object = reinterpret_cast<icontext<pvalue_type&>*>(mapping);
      auto parent = static_cast<icontext<Class>*>(object->parent_);

      bool nullify = object->is_null_;
      parent->parser_state_.pop(object, parent);

      if (nullify)
      {
        parent->obj_ = nullptr;
      }

      if (parent->pop_fn_)
      {
        parent->pop_fn_(parent);
      }
    };
  }

  void read_optional()
  {
    using pvalue_type = typename class_type::value_type;
    if (!obj_)
      obj_.emplace();

    auto mapping     = parser_state_.push<icontext<pvalue_type&>>(*obj_, parser_state_, this);
    mapping->pop_fn_ = [](in_context_base* mapping)
    {
      auto object = reinterpret_cast<icontext<pvalue_type&>*>(mapping);
      auto parent = static_cast<icontext<Class>*>(object->parent_);

      bool nullify = object->is_null_;
      parent->parser_state_.pop(object, parent);

      if (nullify)
      {
        parent->obj_.reset();
      }

      if (parent->pop_fn_)
      {
        parent->pop_fn_(parent);
      }
    };
  }

  void read_tuple()
  {
    read_tuple_value<std::tuple_size_v<class_type> - 1>(xvalue_++);
  }

  void read_variant_type()
  {
    auto mapping     = parser_state_.push<icontext<uint32_t>>(parser_state_, this);
    mapping->pop_fn_ = [](in_context_base* mapping)
    {
      auto object              = static_cast<icontext<uint32_t>*>(mapping);
      object->parent_->xvalue_ = object->get();
      object->parent_->parser_state_.pop(object, object->parent_);
    };
  }

  void read_variant(std::string_view key)
  {
    if (key == "type")
    {
      read_variant_type();
    }
    else if (key == "value")
    {
      read_variant_value<std::variant_size_v<class_type> - 1>(xvalue_);
    }
  }

  template <std::size_t const I>
  void read_variant_value(uint32_t i) noexcept
  {
    if (I == i)
    {
      using type = std::variant_alternative_t<I, class_type>;

      auto mapping     = parser_state_.push<icontext<type>>(parser_state_, this);
      mapping->pop_fn_ = [](in_context_base* mapping)
      {
        auto object = static_cast<icontext<type>*>(mapping);
        auto parent = static_cast<icontext<Class>*>(object->parent_);
        parent->get().template emplace<type>(std::move(object->get()));
        parent->parser_state_.pop(object, parent);
      };
    }
    else if constexpr (I > 0)
      read_variant_value<I - 1>(i);
    else
      return;
  }

  template <std::size_t const I>
  void read_tuple_value(uint32_t i) noexcept
  {
    if (I == i)
    {
      using type = std::tuple_element_t<I, class_type>;

      auto mapping     = parser_state_.push<icontext<type>>(parser_state_, this);
      mapping->pop_fn_ = [](in_context_base* mapping)
      {
        auto object                = static_cast<icontext<type>*>(mapping);
        auto parent                = static_cast<icontext<Class>*>(object->parent_);
        std::get<I>(parent->get()) = std::move(object->get());
        parent->parser_state_.pop(object, parent);
      };
    }
    else if constexpr (I > 0)
      read_tuple_value<I - 1>(i);
    else
      return;
  }

  void push_container_item()
    requires(!ContainerHasEmplaceBack<class_type> && ContainerHasArrayValueAssignable<class_type>)
  {
    auto mapping     = parser_state_.push<icontext<array_value_type<class_type>>>(parser_state_, this);
    mapping->pop_fn_ = [](in_context_base* mapping)
    {
      auto object = static_cast<icontext<array_value_type<class_type>>*>(mapping);
      auto parent = static_cast<icontext<Class>*>(object->parent_);
      if (parent->xvalue_ < parent->get().size())
      {
        parent->get()[parent->xvalue_++] = std::move(object->get());
      }
      parent->parser_state_.pop(object, parent);
    };
  }

  void push_container_item()
    requires(ContainerHasEmplaceBack<class_type>)
  {
    auto mapping     = parser_state_.push<icontext<typename class_type::value_type>>(parser_state_, this);
    mapping->pop_fn_ = [](in_context_base* mapping)
    {
      auto object = static_cast<icontext<typename class_type::value_type>*>(mapping);
      auto parent = static_cast<icontext<Class>*>(object->parent_);
      parent->get().emplace_back(std::move(object->get()));
      parent->parser_state_.pop(object, parent);
    };
  }

  void push_container_item()
    requires(ContainerHasEmplace<class_type> && !MapLike<class_type>)
  {
    using value_t    = typename class_type::value_type;
    auto mapping     = parser_state_.push<icontext<value_t>>(parser_state_, this);
    mapping->pop_fn_ = [](in_context_base* mapping)
    {
      auto object = static_cast<icontext<value_t>*>(mapping);
      auto parent = static_cast<icontext<Class>*>(object->parent_);
      parent->get().emplace(std::move(object->get()));
      parent->parser_state_.pop(object, parent);
    };
  }

  void push_container_item()
    requires(detail::ComplexMapLike<class_type>)
  {
    using value_t    = map_value_type<typename class_type::key_type, typename class_type::mapped_type>;
    auto mapping     = parser_state_.push<icontext<value_t>>(parser_state_, this);
    mapping->pop_fn_ = [](in_context_base* mapping)
    {
      auto  object = static_cast<icontext<value_t>*>(mapping);
      auto  parent = static_cast<icontext<Class>*>(object->parent_);
      auto& pair   = object->get();
      parent->get().emplace(std::move(pair.key), std::move(pair.value));
      parent->parser_state_.pop(object, parent);
    };
  }

  void push_container_item()
    requires(detail::StringMapLike<class_type>)
  {
    using value_t    = string_map_value_type<typename class_type::mapped_type>;
    auto mapping     = parser_state_.push<icontext<value_t>>(parser_state_, this);
    mapping->pop_fn_ = [](in_context_base* mapping)
    {
      auto  object = static_cast<icontext<value_t>*>(mapping);
      auto  parent = static_cast<icontext<Class>*>(object->parent_);
      auto& pair   = object->get();
      parent->get().emplace(std::move(pair.key), std::move(pair.value));
      parent->parser_state_.pop(object, parent);
    };
  }

  void read_string_map_value(std::string_view key)
  {
    obj_.key         = key;
    using value_t    = typename class_type::value_type;
    auto mapping     = parser_state_.push<icontext<value_t&>>(obj_.value, parser_state_, this);
    mapping->pop_fn_ = [](in_context_base* mapping)
    {
      auto object = static_cast<icontext<value_t&>*>(mapping);
      auto parent = static_cast<icontext<Class>*>(object->parent_);
      parent->parser_state_.pop(object, parent);
    };
  }

  void read_bound_class(std::string_view key)
  {
    for_each_field(
      [this, key]<typename Decl>(class_type& obj, Decl const& decl, auto) noexcept
      {
        if (decl.key() == key)
        {
          using value_t = typename Decl::MemTy;

          auto mapping     = parser_state_.push<icontext<value_t>>(parser_state_, this);
          mapping->pop_fn_ = [](in_context_base* mapping)
          {
            auto object = reinterpret_cast<icontext<value_t>*>(mapping);
            auto parent = static_cast<icontext<Class>*>(object->parent_);
            Decl decl;
            decl.value(parent->get(), std::move(object->get()));
            parent->parser_state_.pop(object, parent);
          };
        }
      },
      obj_);
  }

private:
  Class obj_;
};

} // namespace detail

namespace yaml
{
template <typename Class>
void from_string(Class& obj, std::string_view data)
{
  auto state    = detail::parser_state(data);
  auto icontext = detail::icontext<Class&>(obj, state, nullptr);
  state.parse(icontext);
}

} // namespace yaml
} // namespace acl