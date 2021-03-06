#include <lightstep/impl.h>
#include <lightstep/tracer.h>
#include <ngx_opentracing_utility.h>

extern "C" {
#include <nginx.h>
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
}

namespace ngx_opentracing {
//------------------------------------------------------------------------------
// insert_header
//------------------------------------------------------------------------------
static bool insert_header(ngx_http_request_t *request, ngx_str_t key,
                          ngx_str_t value) {
  auto header = static_cast<ngx_table_elt_t *>(
      ngx_list_push(&request->headers_in.headers));
  if (!header) return false;
  header->hash = 1;
  header->key = key;
  header->lowcase_key = key.data;
  header->value = value;
  return true;
}

//------------------------------------------------------------------------------
// set_headers
//------------------------------------------------------------------------------
static bool set_headers(ngx_http_request_t *request,
                        std::vector<std::pair<ngx_str_t, ngx_str_t>> &headers) {
  if (headers.empty()) return true;

  // If header keys are already in the request, overwrite the values instead of
  // inserting a new header.
  //
  // It may be possible in some cases to use nginx's hashes to look up the
  // entries faster, but then we'd have to handle the special case of when a
  // header element isn't hashed yet. Iterating over the header entries all the
  // time keeps things simple.
  for_each<ngx_table_elt_t>(
      request->headers_in.headers, [&](ngx_table_elt_t &header) {
        auto i = std::find_if(
            headers.begin(), headers.end(),
            [&](const std::pair<ngx_str_t, ngx_str_t> &key_value) {
              const auto &key = key_value.first;
              return header.key.len == key.len &&
                     ngx_strncmp(reinterpret_cast<char *>(header.lowcase_key),
                                 reinterpret_cast<char *>(key.data),
                                 key.len) == 0;

            });
        if (i == headers.end()) return;
        ngx_log_debug4(
            NGX_LOG_DEBUG_HTTP, request->connection->log, 0,
            "replacing opentracing header \"%V:%V\" with value \"%V\""
            " in request %p",
            &header.key, &header.value, &i->second, request);
        header.value = i->second;
        headers.erase(i);
      });

  // Any header left in `headers` doesn't already have a key in the request, so
  // create a new entry for it.
  for (const auto &key_value : headers) {
    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, request->connection->log, 0,
                   "adding opentracing header \"%V:%V\" in request %p",
                   &key_value.first, &key_value.second, request);
    if (!insert_header(request, key_value.first, key_value.second)) {
      ngx_log_error(NGX_LOG_ERR, request->connection->log, 0,
                    "failed to insert header");
      return false;
    }
  }
  return true;
}

//------------------------------------------------------------------------------
// NgxHeaderCarrierWriter
//------------------------------------------------------------------------------
namespace {
class NgxHeaderCarrierWriter : public lightstep::BasicCarrierWriter {
 public:
  NgxHeaderCarrierWriter(ngx_http_request_t *request,
                         std::vector<std::pair<ngx_str_t, ngx_str_t>> &headers,
                         bool &was_successful)
      : request_{request}, headers_{headers}, was_successful_{was_successful} {
    was_successful_ = true;
  }

  void Set(const std::string &key, const std::string &value) const override {
    if (!was_successful_) return;
    auto ngx_key = to_lower_ngx_str(request_->pool, key);
    if (!ngx_key.data) {
      ngx_log_error(NGX_LOG_ERR, request_->connection->log, 0,
                    "failed to allocate header key");
      was_successful_ = false;
      return;
    }
    auto ngx_value = to_ngx_str(request_->pool, value);
    if (!ngx_value.data) {
      ngx_log_error(NGX_LOG_ERR, request_->connection->log, 0,
                    "failed to allocate header value");
      was_successful_ = false;
      return;
    }
    headers_.emplace_back(ngx_key, ngx_value);
  }

 private:
  ngx_http_request_t *request_;
  std::vector<std::pair<ngx_str_t, ngx_str_t>> &headers_;
  bool &was_successful_;
};
}

//------------------------------------------------------------------------------
// inject_span_context
//------------------------------------------------------------------------------
void inject_span_context(lightstep::Tracer &tracer, ngx_http_request_t *request,
                         const lightstep::SpanContext &span_context) {
  ngx_log_debug3(NGX_LOG_DEBUG_HTTP, request->connection->log, 0,
                 "injecting opentracing span context (trace_id=%uxL"
                 ", span_id=%uxL) in request %p",
                 span_context.trace_id(), span_context.span_id(), request);
  std::vector<std::pair<ngx_str_t, ngx_str_t>> headers;
  bool was_successful = true;
  auto carrier_writer =
      NgxHeaderCarrierWriter{request, headers, was_successful};
  auto successfully_injected = tracer.Inject(
      span_context, lightstep::CarrierFormat::HTTPHeaders, carrier_writer);
  was_successful = was_successful && successfully_injected;
  if (was_successful) was_successful = set_headers(request, headers);
  if (!was_successful)
    ngx_log_error(NGX_LOG_ERR, request->connection->log, 0,
                  "Tracer.inject() failed");
}
}  // namespace ngx_opentracing
