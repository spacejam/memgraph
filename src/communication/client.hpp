#pragma once

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "communication/buffer.hpp"
#include "communication/context.hpp"
#include "communication/init.hpp"
#include "io/network/endpoint.hpp"
#include "io/network/socket.hpp"

namespace communication {

/**
 * This class implements a generic network Client.
 * It uses blocking sockets and provides an API that can be used to receive/send
 * data over the network connection.
 *
 * NOTE: If you use this client you **must** call `communication::Init()` from
 * the `main` function before using the client!
 */
class Client final {
 public:
  explicit Client(ClientContext *context);

  ~Client();

  Client(const Client &) = delete;
  Client(Client &&) = delete;
  Client &operator=(const Client &) = delete;
  Client &operator=(Client &&) = delete;

  /**
   * This function connects to a remote server and returns whether the connect
   * succeeded.
   */
  bool Connect(const io::network::Endpoint &endpoint);

  /**
   * This function returns `true` if the socket is in an error state.
   */
  bool ErrorStatus();

  /**
   * This function shuts down the socket.
   */
  void Shutdown();

  /**
   * This function closes the socket.
   */
  void Close();

  /**
   * This function is used to receive `len` bytes from the socket and stores it
   * in an internal buffer. It returns `true` if the read succeeded and `false`
   * if it didn't.
   */
  bool Read(size_t len);

  /**
   * This function returns a pointer to the read data that is currently stored
   * in the client.
   */
  uint8_t *GetData();

  /**
   * This function returns the size of the read data that is currently stored in
   * the client.
   */
  size_t GetDataSize();

  /**
   * This function removes first `len` bytes from the data buffer.
   */
  void ShiftData(size_t len);

  /**
   * This function clears the data buffer.
   */
  void ClearData();

  /**
   * This function writes data to the socket.
   * TODO (mferencevic): the `have_more` flag currently isn't supported when
   * using OpenSSL
   */
  bool Write(const uint8_t *data, size_t len, bool have_more = false);

  /**
   * This function writes data to the socket.
   */
  bool Write(const std::string &str, bool have_more = false);

  const io::network::Endpoint &endpoint();

 private:
  void ReleaseSslObjects();

  io::network::Socket socket_;
  Buffer buffer_;

  ClientContext *context_;
  SSL *ssl_{nullptr};
  BIO *bio_{nullptr};
};

/**
 * This class provides a stream-like input side object to the client.
 */
class ClientInputStream final {
 public:
  ClientInputStream(Client &client);

  ClientInputStream(const ClientInputStream &) = delete;
  ClientInputStream(ClientInputStream &&) = delete;
  ClientInputStream &operator=(const ClientInputStream &) = delete;
  ClientInputStream &operator=(ClientInputStream &&) = delete;

  uint8_t *data();

  size_t size() const;

  void Shift(size_t len);

  void Clear();

 private:
  Client &client_;
};

/**
 * This class provides a stream-like output side object to the client.
 */
class ClientOutputStream final {
 public:
  ClientOutputStream(Client &client);

  ClientOutputStream(const ClientOutputStream &) = delete;
  ClientOutputStream(ClientOutputStream &&) = delete;
  ClientOutputStream &operator=(const ClientOutputStream &) = delete;
  ClientOutputStream &operator=(ClientOutputStream &&) = delete;

  bool Write(const uint8_t *data, size_t len, bool have_more = false);

  bool Write(const std::string &str, bool have_more = false);

 private:
  Client &client_;
};

}  // namespace communication
