// Copyright 2011 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mod_spdy/apache/filters/spdy_to_http_filter.h"

#include <string>

#include "httpd.h"
#include "apr_buckets.h"
#include "apr_tables.h"
#include "util_filter.h"

#include "base/string_piece.h"
#include "mod_spdy/apache/pool_util.h"
#include "mod_spdy/common/spdy_frame_priority_queue.h"
#include "mod_spdy/common/spdy_stream.h"
#include "net/spdy/spdy_framer.h"
#include "net/spdy/spdy_protocol.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

class SpdyToHttpFilterTest : public testing::Test {
 public:
  SpdyToHttpFilterTest()
      : stream_id_(1),
        priority_(SPDY_PRIORITY_HIGHEST),
        stream_(stream_id_, priority_, &output_queue_),
        spdy_to_http_filter_(&stream_) {
    bucket_alloc_ = apr_bucket_alloc_create(local_.pool());
    connection_ = static_cast<conn_rec*>(
        apr_pcalloc(local_.pool(), sizeof(conn_rec)));
    connection_->pool = local_.pool();
    connection_->bucket_alloc = bucket_alloc_;
    ap_filter_ = static_cast<ap_filter_t*>(
        apr_pcalloc(local_.pool(), sizeof(ap_filter_t)));
    ap_filter_->c = connection_;
    brigade_ = apr_brigade_create(local_.pool(), bucket_alloc_);
  }

 protected:
  void PostSynStream(spdy::SpdyControlFlags flags,
                     spdy::SpdyHeaderBlock* headers) {
    stream_.PostInputFrame(framer_.CreateSynStream(
        stream_id_,
        0,  // associated_stream_id
        priority_, flags,
        false, // compressed
        headers));
  }

  void PostDataFrame(spdy::SpdyDataFlags flags,
                     const base::StringPiece& payload) {
    stream_.PostInputFrame(framer_.CreateDataFrame(
        stream_id_, payload.data(), payload.size(), flags));
  }

  apr_status_t Read(ap_input_mode_t mode, apr_read_type_e block,
                    apr_off_t readbytes) {
    return spdy_to_http_filter_.Read(ap_filter_, brigade_,
                                     mode, block, readbytes);
  }

  void ExpectTransientBucket(const std::string& expected) {
    ASSERT_FALSE(APR_BRIGADE_EMPTY(brigade_))
        << "Expected TRANSIENT bucket, but brigade is empty.";
    apr_bucket* bucket = APR_BRIGADE_FIRST(brigade_);
    ASSERT_TRUE(APR_BUCKET_IS_TRANSIENT(bucket))
        << "Expected TRANSIENT bucket, but found " << bucket->type->name
        << " bucket.";
    const char* data = NULL;
    apr_size_t size = 0;
    ASSERT_EQ(APR_SUCCESS, apr_bucket_read(
        bucket, &data, &size, APR_NONBLOCK_READ));
    EXPECT_EQ(expected, std::string(data, size));
    apr_bucket_delete(bucket);
  }

  void ExpectEosBucket() {
    ASSERT_FALSE(APR_BRIGADE_EMPTY(brigade_))
        << "Expected EOS bucket, but brigade is empty.";
    apr_bucket* bucket = APR_BRIGADE_FIRST(brigade_);
    ASSERT_TRUE(APR_BUCKET_IS_EOS(bucket))
        << "Expected EOS bucket, but found " << bucket->type->name
        << " bucket.";
    apr_bucket_delete(bucket);
  }

  void ExpectEndOfBrigade() {
    ASSERT_TRUE(APR_BRIGADE_EMPTY(brigade_))
        << "Expected brigade to be empty, but found "
        << APR_BRIGADE_FIRST(brigade_)->type->name << " bucket.";
    ASSERT_EQ(APR_SUCCESS, apr_brigade_cleanup(brigade_));
  }

  const spdy::SpdyStreamId stream_id_;
  const spdy::SpdyPriority priority_;
  spdy::SpdyFramer framer_;
  mod_spdy::SpdyFramePriorityQueue output_queue_;
  mod_spdy::SpdyStream stream_;
  mod_spdy::SpdyToHttpFilter spdy_to_http_filter_;

  mod_spdy::LocalPool local_;
  apr_bucket_alloc_t* bucket_alloc_;
  conn_rec* connection_;
  ap_filter_t* ap_filter_;
  apr_bucket_brigade* brigade_;
};

TEST_F(SpdyToHttpFilterTest, SimpleGetRequest) {
  // Perform an INIT.  It should succeed, with no effect.
  ASSERT_EQ(APR_SUCCESS, Read(AP_MODE_INIT, APR_BLOCK_READ, 1337));
  ExpectEndOfBrigade();

  // Invoke the fitler in non-blocking GETLINE mode.  We shouldn't get anything
  // yet, because we haven't sent any frames from the client yet.
  ASSERT_TRUE(APR_STATUS_IS_EAGAIN(
      Read(AP_MODE_GETLINE, APR_NONBLOCK_READ, 0)));
  ExpectEndOfBrigade();

  // Send a SYN_STREAM frame from the client, with FLAG_FIN set.
  spdy::SpdyHeaderBlock headers;
  headers["accept-charset"] = "utf8";
  headers["accept-language"] = "en";
  headers["host"] = "www.example.com";
  headers["method"] = "GET";
  headers["referer"] = "https://www.example.com/index.html";
  headers["scheme"] = "https";
  headers["url"] = "/foo/bar/index.html";
  headers["user-agent"] = "ModSpdyUnitTest/1.0";
  headers["version"] = "HTTP/1.1";
  PostSynStream(spdy::CONTROL_FLAG_FIN, &headers);

  // Invoke the filter in blocking GETLINE mode.  We should get back just the
  // HTTP request line.
  ASSERT_EQ(APR_SUCCESS, Read(AP_MODE_GETLINE, APR_BLOCK_READ, 0));
  ExpectTransientBucket("GET /foo/bar/index.html HTTP/1.1\r\n");
  ExpectEndOfBrigade();

  // Now do a SPECULATIVE read.  We should get back a few bytes.
  ASSERT_EQ(APR_SUCCESS, Read(AP_MODE_SPECULATIVE, APR_NONBLOCK_READ, 8));
  ExpectTransientBucket("accept-c");
  ExpectEndOfBrigade();

  // Now do another GETLINE read.  We should get back the first header line,
  // including the data we just read speculatively.
  ASSERT_EQ(APR_SUCCESS, Read(AP_MODE_GETLINE, APR_NONBLOCK_READ, 0));
  ExpectTransientBucket("accept-charset: utf8\r\n");
  ExpectEndOfBrigade();

  // Do a READBYTES read.  We should get back a few bytes.
  ASSERT_EQ(APR_SUCCESS, Read(AP_MODE_READBYTES, APR_NONBLOCK_READ, 12));
  ExpectTransientBucket("accept-langu");
  ExpectEndOfBrigade();

  // Do another GETLINE read.  We should get back the rest of the header line,
  // *not* including the data we just read.
  ASSERT_EQ(APR_SUCCESS, Read(AP_MODE_GETLINE, APR_NONBLOCK_READ, 0));
  ExpectTransientBucket("age: en\r\n");
  ExpectEndOfBrigade();

  // Finally, do an EXHAUSTIVE read.  We should get back everything that
  // remains, terminating with an EOS bucket.
  ASSERT_EQ(APR_SUCCESS, Read(AP_MODE_EXHAUSTIVE, APR_NONBLOCK_READ, 0));
  ExpectTransientBucket("host: www.example.com\r\n"
                        "referer: https://www.example.com/index.html\r\n"
                        "user-agent: ModSpdyUnitTest/1.0\r\n"
                        "\r\n");
  ExpectEosBucket();
  ExpectEndOfBrigade();

  // There's no more data left; attempting another read should result in EOF.
  ASSERT_TRUE(APR_STATUS_IS_EOF(
      Read(AP_MODE_READBYTES, APR_NONBLOCK_READ, 4)));
}

TEST_F(SpdyToHttpFilterTest, SimplePostRequest) {
  // Send a SYN_STREAM frame from the client.
  spdy::SpdyHeaderBlock headers;
  headers["host"] = "www.example.com";
  headers["method"] = "POST";
  headers["referer"] = "https://www.example.com/index.html";
  headers["scheme"] = "https";
  headers["url"] = "/erase/the/whole/database.cgi";
  headers["user-agent"] = "ModSpdyUnitTest/1.0";
  headers["version"] = "HTTP/1.1";
  PostSynStream(spdy::CONTROL_FLAG_NONE, &headers);

  // Do a nonblocking READBYTES read.  We ask for lots of bytes, but since it's
  // nonblocking we should immediately get back what's available so far.
  ASSERT_EQ(APR_SUCCESS, Read(AP_MODE_READBYTES, APR_NONBLOCK_READ, 4096));
  ExpectTransientBucket("POST /erase/the/whole/database.cgi HTTP/1.1\r\n"
                        "host: www.example.com\r\n"
                        "referer: https://www.example.com/index.html\r\n"
                        "user-agent: ModSpdyUnitTest/1.0\r\n"
                        "\r\n");
  ExpectEndOfBrigade();

  // There's nothing more available yet, so a nonblocking read should fail.
  ASSERT_TRUE(APR_STATUS_IS_EAGAIN(
      Read(AP_MODE_READBYTES, APR_NONBLOCK_READ, 4)));
  ExpectEndOfBrigade();

  // Send some DATA frames.
  PostDataFrame(spdy::DATA_FLAG_NONE, "Hello, world!\nPlease erase ");
  PostDataFrame(spdy::DATA_FLAG_NONE, "the whole database ");
  PostDataFrame(spdy::DATA_FLAG_FIN, "immediately.\nThanks!\n");

  // Now read in the data a bit at a time.
  ASSERT_EQ(APR_SUCCESS, Read(AP_MODE_GETLINE, APR_NONBLOCK_READ, 0));
  ExpectTransientBucket("Hello, world!\n");
  ExpectEndOfBrigade();
  ASSERT_EQ(APR_SUCCESS, Read(AP_MODE_READBYTES, APR_NONBLOCK_READ, 24));
  ExpectTransientBucket("Please erase the whole d");
  ExpectEndOfBrigade();
  ASSERT_EQ(APR_SUCCESS, Read(AP_MODE_SPECULATIVE, APR_NONBLOCK_READ, 12));
  ExpectTransientBucket("atabase imme");
  ExpectEndOfBrigade();
  ASSERT_EQ(APR_SUCCESS, Read(AP_MODE_READBYTES, APR_NONBLOCK_READ, 24));
  ExpectTransientBucket("atabase immediately.\nTha");
  ExpectEndOfBrigade();
  ASSERT_EQ(APR_SUCCESS, Read(AP_MODE_GETLINE, APR_NONBLOCK_READ, 0));
  ExpectTransientBucket("nks!\n");
  ExpectEosBucket();
  ExpectEndOfBrigade();

  // There's no more data left; attempting another read should result in EOF.
  ASSERT_TRUE(APR_STATUS_IS_EOF(Read(AP_MODE_GETLINE, APR_BLOCK_READ, 0)));
}

}  // namespace
