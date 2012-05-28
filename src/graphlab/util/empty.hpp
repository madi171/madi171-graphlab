#ifndef GRAPHLAB_UTIL_EMPTY_HPP
#define GRAPHLAB_UTIL_EMPTY_HPP
#include <vector>
#include <algorithm>
#include <stdexcept>
namespace graphlab {

struct empty{};

} // namespace graphlab;


namespace std {

template <>
class vector<graphlab::empty, allocator<graphlab::empty> > {
 private:
  size_t len;
  graphlab::empty e;

 public:
  struct iterator {
    typedef int difference_type;
    typedef graphlab::empty value_type;
    typedef graphlab::empty* pointer;
    typedef graphlab::empty& reference;
    typedef const graphlab::empty& const_reference;
    typedef random_access_iterator_tag iterator_category;
    
    graphlab::empty e;
    size_t i; const size_t* len;
    iterator(): i(0),len(NULL) { }
    iterator(size_t i, const size_t* len): i(i), len(len) { }
    bool operator==(const iterator& iter) const {
      return i == iter.i;
    }
    bool operator!=(const iterator& iter) const {
      return !((*this) == iter);
    }
    iterator operator++() {
      i += (i < *len);
      return *this;
    }
    reference operator*() {
      return e;
    }
    const_reference operator*() const {
      return e;
    }
    iterator operator++(int) {
      iterator old = (*this);
      i += (i < *len);
      return old;
    }
    iterator operator--() {
      i -= (i > 0);
      return *this;
    }
    iterator operator--(int) {
      iterator old = (*this);
      i -= (i > 0);
      return old;
    }
    iterator operator+=(int n) {
      i += n;
      if (n > 0 && i > (*len)) i = (*len); // overflow
      else if (n < 0 && i > (*len)) i = 0; // underflow
      return *this;
    }
    iterator operator-=(int n) {
      i += -n;
      return *this;
    }
    iterator operator+(int n) const {
      iterator tmp = (*this);
      tmp += n;
      return tmp;
    }
    iterator operator-(int n) const {
      iterator tmp = (*this);
      tmp -= n;
      return tmp;
    }

    int operator-(iterator n) const {
      return i - n.i;
    }

    bool operator<(iterator other) const {
      return i < other.i;
    }
    bool operator<=(iterator other) const {
      return i <= other.i;
    }
    bool operator>(iterator other) const {
      return i > other.i;
    }
    bool operator>=(iterator other) const {
      return i >= other.i;
    }
  };
  
  typedef iterator const_iterator;
  typedef iterator reverse_iterator;
  typedef iterator const_reverse_iterator;
  typedef graphlab::empty& reference;
  typedef const graphlab::empty& const_reference;
  typedef allocator<graphlab::empty> allocator_type;

  // default constructor
  explicit vector(const allocator_type& a = allocator_type()):len(0) { }
  // construct from value
  explicit vector(size_t n, const graphlab::empty& val = graphlab::empty(),
           const allocator_type& a = allocator_type()):len(n) { }
  // construct from iterator
  template <typename InputIterator>
  vector(InputIterator first, InputIterator last, 
         const allocator_type& a = allocator_type()): len(std::distance(first, last)) { }
  // copy constructor
  vector(const vector<graphlab::empty, allocator_type>& v): len(v.len) { };

  iterator begin() {
    return iterator(0, &len);
  }
  iterator end() {
    return iterator(len, &len);
  }
  const_iterator begin() const {
    return const_iterator(0, &len);
  }
  const_iterator end() const {
    return const_iterator(len, &len);
  }

  reverse_iterator rbegin() {
    return iterator(0, &len);
  }
  reverse_iterator rend() {
    return iterator(len, &len);
  }
  const_reverse_iterator rbegin() const {
    return const_iterator(0, &len);
  }
  const_reverse_iterator rend() const {
    return const_iterator(len, &len);
  }

  size_t size() const {
    return len;
  }
  
  size_t capacity() const {
    return len;
  }

  bool empty() const {
    return len == 0;
  }

  void resize(size_t s) { len = s; }

  void reserve(size_t s) {}

  reference operator[](int i) {
    return e;
  }

  const_reference operator[](int i) const {
    return e;
  }

  reference at(int i) {
    if (i < 0 || (size_t)i >= len) throw std::out_of_range("vector index out of range");
    else return e;
  }

  const_reference at(int i) const {
    if (i < 0 || (size_t)i >= len) throw std::out_of_range("vector index out of range");
    else return e;
  }

  template <typename InputIterator>
  void assign(InputIterator first, InputIterator last) {
    len = std::distance(first, last);
  }


  void assign(size_t n, const graphlab::empty&) {
    len = n;
  }

  void push_back(const graphlab::empty&) {
    ++len;
  }

  void pop_back(const graphlab::empty&) {
    --len;
  }

  void insert(iterator, const graphlab::empty&) {
    ++len;
  }


  void insert(iterator, size_t n, const graphlab::empty&) {
    len += n;
  }


  template <typename InputIterator>
  void insert(iterator, InputIterator first, InputIterator last) {
    len += std::distance(first, last);
  }

  void erase(iterator) {
    --len;
  }

  void erase(iterator first, iterator last) {
    len -= (last - first);
  }

  void swap(vector<graphlab::empty, allocator_type> &v) {
    std::swap(len, v.len);
  }

  void clear() {
    len = 0;
  }

  allocator_type get_allocator() const {
    return allocator_type();
  }
};
  
} // namespace std

#endif