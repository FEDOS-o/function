#pragma once

#include <iostream>
#include <memory>

using storage_t = std::aligned_storage_t<sizeof(void*), alignof(void*)>;

template <typename T>
constexpr T* ptr_from_storage(storage_t* storage) {
  return static_cast<T*>(*reinterpret_cast<void**>(storage));
}

template <typename T>
constexpr const T* ptr_from_storage(const storage_t* storage) {
  return static_cast<const T*>(*reinterpret_cast<void* const*>(storage));
}

template <typename T>
constexpr bool fits_small() {
  return sizeof(T) < sizeof(void*) && alignof(T) < alignof(void*) &&
         std::is_nothrow_move_assignable_v<T> &&
         std::is_nothrow_move_constructible_v<T>;
}

struct bad_function_call {};

template <typename R, typename... Args>
struct type_descriptor {
  void (*copy)(const storage_t* src, storage_t* dst); // pointer to func
  void (*move)(storage_t* src, storage_t* dst);
  void (*destroy)(storage_t* src);
  auto(*get)(const storage_t* src) -> void*;
  R (*invoke)(const storage_t* src, Args... args);

  static const type_descriptor<R, Args...>*
  get_empty_func_descriptor() noexcept {
    constexpr static type_descriptor<R, Args...> result = {
        [](const storage_t* src, storage_t* dst) {},
        [](storage_t* src, storage_t* dst) {}, [](storage_t* src) {},
        [](const storage_t* src) { return static_cast<void*>(nullptr); },
        [](const storage_t* src, Args... args) -> R {
          throw bad_function_call{};
        }};
    return &result;
  }

  template <typename T>
  static const type_descriptor<R, Args...>* get_descriptor() noexcept {

    constexpr static type_descriptor<R, Args...> result = {
        [](const storage_t* src, storage_t* dst) {
          if constexpr (fits_small<T>()) {
            new (dst) T(reinterpret_cast<const T&>(*src));
          } else {
            *reinterpret_cast<void**>(dst) = new T(*ptr_from_storage<T>(src));
          }
        },
        [](storage_t* src, storage_t* dst) {
          // move T from src to dst
          if constexpr (fits_small<T>()) {
            reinterpret_cast<T&>(*dst) = std::move(reinterpret_cast<T&>(*src));
            ptr_from_storage<T>(src)->~T();
          } else {
            *dst = *src;
          }
        },
        [](storage_t* src) {
          // destroy T from src
          if constexpr (fits_small<T>()) {
            reinterpret_cast<T&>(*src).~T();
          } else {
            delete ptr_from_storage<T>(src);
          }
        },
        [](const storage_t* src) -> void* {
          if constexpr (fits_small<T>()) {
            return reinterpret_cast<T*>(const_cast<storage_t*>(src));
          } else {
            return const_cast<T*>(ptr_from_storage<T>(src));
          }
        },
        [](const storage_t* src, Args... args) -> R {
          // invoke T from src with args
          if constexpr (fits_small<T>()) {
            return reinterpret_cast<T const&>(*src)(
                std::forward<Args>(args)...);
          } else {
            return (*ptr_from_storage<T>(src))(std::forward<Args>(args)...);
          }
        }};
    return &result;
  }
};

template <typename F>
struct function;

template <typename R, typename... Args>
struct function<R(Args...)> {
  function() noexcept
      : desc(type_descriptor<R, Args...>::get_empty_func_descriptor()) {}

  function(function const& other) : desc(other.desc) {
    other.desc->copy(&other.storage, &storage);
  }
  function(function&& other) noexcept {
    desc = other.desc;
    desc->move(&other.storage, &storage);
    other.desc = type_descriptor<R, Args...>::get_empty_func_descriptor();
  }

  template <typename T>
  function(T val)
      : desc(type_descriptor<R, Args...>::template get_descriptor<T>()) {
    if constexpr (fits_small<T>()) {
      new (&storage) T(std::move(val));
    } else {
      *reinterpret_cast<void**>(&storage) = new T(std::move(val));
    }
  }

  function& operator=(function const& rhs) {
    if (this != &rhs) {
      function(rhs).swap(*this);
    }
    return *this;
  }

  function& operator=(function&& rhs) noexcept {
    if (this != &rhs) {
      swap(rhs);
      function().swap(rhs);
    }
    return *this;
  }

  ~function() {
    desc->destroy(&storage);
  }

  explicit operator bool() const noexcept {
    return desc != type_descriptor<R, Args...>::get_empty_func_descriptor();
  }

  R operator()(Args... args) const {
    return desc->invoke(&storage, std::forward<Args>(args)...);
  }

  template <typename T>
  T* target() noexcept {
    if (desc != type_descriptor<R, Args...>::template get_descriptor<T>()) {
      return nullptr;
    }
    return reinterpret_cast<T*>(desc->get(&storage));
  }

  template <typename T>
  T const* target() const noexcept {
    if (desc != type_descriptor<R, Args...>::template get_descriptor<T>()) {
      return nullptr;
    }
    return reinterpret_cast<T const*>(desc->get(&storage));
  }

  void swap(function& other) {
    using std::swap;
    storage_t tmp;
    other.desc->move(&other.storage, &tmp);
    desc->move(&storage, &other.storage);
    other.desc->move(&tmp, &storage);
    swap(desc, other.desc);
    /*storage_t tmp_this, tmp_other;
    desc->move(&storage, &tmp_this);
    other.desc->move(&other.storage, &tmp_other);
    desc->move(&tmp_this, &other.storage);
    other.desc->move(&tmp_other, &storage);
    swap(desc, other.desc);*/
    // c одной tmp чет не получалось :(
    /* swap(storage, other.storage); // wrong?
     swap(desc, other.desc);*/
  }

private:
  storage_t storage; // 8 bytes, char[8]
  const type_descriptor<R, Args...>* desc;
};
