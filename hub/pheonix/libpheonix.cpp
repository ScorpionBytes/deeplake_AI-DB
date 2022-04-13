#include <torch/extension.h>

#include <boost/asio.hpp>
#include <iostream>
#include <vector>

#include <coroutine>
#include <curl/curl.h>

#include <concepts>
#include <exception>
#include <iostream>

#include <scheduler.h>

static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

auto simple_request(int a) {
  CURL *curl;
  CURLcode res;
  std::string readBuffer;

  curl = curl_easy_init();
  if (curl){
    curl_easy_setopt(curl, CURLOPT_URL, "http://www.google.com");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
    res = curl_easy_perform(curl); //we really want to make this awaitable
    curl_easy_cleanup(curl);
  }

  //co_return py::bytes(readBuffer);
  return py::bytes(readBuffer);
}

class PrefetchIterator{
private: 
  int a;
  std::vector<int> v;
  py::object ref; // keep a reference
  size_t index = 0;

public:
  PrefetchIterator(){
    //myCoroutineResult = myCoroutineFunction();
    v = {1, 2, 3};
  }

  int next(){
    if (index == v.size())
      throw py::stop_iteration(); // Great
    return v[index++]; //(myCoroutineResult.next());
  }

};

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("simple_request", &simple_request, "simple request");
  pybind11::class_<PrefetchIterator>(m, "prefetch")
            .def(pybind11::init())
            .def("__next__", &PrefetchIterator::next)
            .def("__iter__", [](PrefetchIterator &it) -> PrefetchIterator& { return it; }, py::keep_alive<0, 1>());
}
