#pragma once

template<class Tag> struct Profile {
  static constexpr int limit = Tag::number;
};
