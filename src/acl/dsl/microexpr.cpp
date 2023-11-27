
#include <acl/dsl/microexpr.hpp>

namespace acl
{
bool microexpr::evaluate(std::string_view expr)
{
  content_ = expr;
  read_    = 0;
  return conditional() != 0;
}

void microexpr::skip_white() noexcept
{
  auto n = content_.find_first_not_of(" \t\r\n", read_);
  if (n != content_.npos)
    read_ = (uint32_t)n;
}

int64_t microexpr::conditional()
{
  int64_t left = comparison();
  skip_white();
  if (read_ >= content_.length() || content_[read_] != '?')
    return left;
  read_++;
  int64_t op_a = comparison();
  skip_white();
  if (read_ >= content_.length() || content_[read_] != ':')
    return left;
  read_++;
  int64_t op_b = comparison();
  return left ? op_a : op_b;
}

int64_t microexpr::comparison()
{
  int64_t left = binary();
  skip_white();
  if (read_ >= content_.length())
    return left;

  char sv[2] = {content_[read_], 0};
  if (read_ + 1 < content_.length())
    sv[1] = content_[read_ + 1];

  if (sv[0] == '=' && sv[1] == '=')
  {
    read_ += 2;
    return (left == binary());
  }
  else if (sv[0] == '!' && sv[1] == '=')
  {
    read_ += 2;
    return left != binary();
  }
  else if (sv[0] == '<' && sv[1] == '=')
  {
    read_ += 2;
    return left <= binary();
  }
  else if (sv[0] == '>' && sv[1] == '=')
  {
    read_ += 2;
    return left >= binary();
  }
  else if (sv[0] == '>')
  {
    read_++;
    return left > binary();
  }
  else if (sv[0] == '<')
  {
    read_++;
    return left < binary();
  }
  else
  {
    return left;
  }
}

int64_t microexpr::binary()
{
  int64_t left = unary();
  while (true)
  {
    skip_white();
    if (read_ >= content_.length())
      return left;

    char sv[2] = {content_[read_], 0};
    if (read_ + 1 < content_.length())
      sv[1] = content_[read_ + 1];

    if (sv[0] == '&' && sv[1] == '&')
    {
      read_ += 2;
      left = (left && unary());
    }
    else if (sv[0] == '|' && sv[1] == '|')
    {
      read_ += 2;
      return (left || unary());
    }
    else if (sv[0] == '&')
    {
      read_++;
      left = left & unary();
    }
    else if (sv[0] == '|')
    {
      read_++;
      return left | unary();
    }
    else if (sv[0] == '^')
    {
      read_++;
      left = left ^ unary();
    }
    else if (sv[0] == '+')
    {
      read_++;
      left = left + unary();
    }
    else if (sv[0] == '-')
    {
      read_++;
      left = left - unary();
    }
    else if (sv[0] == '*')
    {
      read_++;
      left = left * unary();
    }
    else if (sv[0] == '/')
    {
      read_++;
      left = left / unary();
    }
    else if (sv[0] == '%')
    {
      read_++;
      left = left % unary();
    }
    else
    {
      return left;
    }
  }
}

std::string_view microexpr::read_token() noexcept
{
  std::string_view ret;
  uint32_t         t = read_;
  while (t < content_.size() && std::isalnum(content_[t]))
    t++;

  return content_.substr(read_, t - read_);
}

int64_t microexpr::unary()
{
  skip_white();
  if (read_ < content_.length())
  {
    char op = get();
    if (op == '(')
    {
      read_++;
      int64_t result = conditional();
      op             = get();
      if (op != ')')
        return 0;
      read_++;
      return result;
    }
    else if (op == '-')
    {
      read_++;
      return -unary();
    }
    else if (op == '~')
    {
      read_++;
      return ~unary();
    }
    else if (std::isdigit(op))
    {
      auto tk = read_token();
      read_ += (uint32_t)tk.length();
      uint64_t value = 0;
      if (tk.starts_with("0x"))
        std::from_chars(tk.data() + 2, tk.data() + tk.size(), value, 16);
      else if (tk.starts_with("0"))
        std::from_chars(tk.data() + 1, tk.data() + tk.size(), value, 8);
      else
        std::from_chars(tk.data(), tk.data() + tk.size(), value);
      return (int64_t)value;
    }
    else if (op == '$')
    {
      read_++;
      skip_white();
      auto tk = read_token();
      read_ += (uint32_t)tk.length();
      return ctx_(tk).has_value();
    }
    else if (isalpha(op) || op == '_')
    {
      auto tk = read_token();
      read_ += (uint32_t)tk.length();
      auto v = ctx_(tk);
      return v.has_value() ? v.value() : 0;
    }
  }
  return 0;
}

} // namespace acl