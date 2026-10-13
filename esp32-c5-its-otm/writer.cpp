#include "esp32-c5-its-otm.h"

const std::string statusOnline("online") ;
const std::string statusOffline("offline") ;

Writer::Writer(QueueIts &queueIts) : mosqpp::mosquittopp(options._mqttId.c_str()), _queueIts(queueIts)
{
}

bool Writer::start()
{
  mosqpp::lib_init() ;

  // publish() from other threads only queues, networkLoop() sends
  int rc = threaded_set(true);
  if (rc != MOSQ_ERR_SUCCESS) {
    std::cerr << "mqtt threaded set failed: " << mosqpp::strerror(rc) << std::endl;
    return false ;
  }

  if (options._mqttTopicStatus.size())
  {
    will_set(options._mqttTopicStatus.c_str(), static_cast<int>(statusOffline.size()), statusOffline.c_str(), 1, true) ;
  }

  std::string caFile = options._caFile ;
  if (options._tls && caFile.empty())
  {
    std::vector<std::string> paths{"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem"} ;
    caFile = paths[0] ;
    for (const auto &path : paths)
    {
      if (access(path.c_str(), R_OK) == 0)
      {
        caFile = path ;
        break ;
      }
    }
  }

  switch (options._tls)
  {
    default:
      break ;
    case 1:
      rc = tls_set(caFile.c_str(), nullptr, nullptr, nullptr, nullptr);
      break ;
    case 2:
      rc = tls_set(caFile.c_str(), nullptr, nullptr, nullptr, nullptr);
      if (rc == MOSQ_ERR_SUCCESS)
        rc = tls_opts_set(SSL_VERIFY_NONE, nullptr, nullptr);
      if (rc == MOSQ_ERR_SUCCESS)
        rc = tls_insecure_set(true);
      break ;
  }
  if (rc != MOSQ_ERR_SUCCESS)
  {
    std::cerr << "tls setup failed (ca file " << caFile << "): " << mosqpp::strerror(rc) << std::endl;
    return false ;
  }
  rc = connect(options._mqttHost.c_str(), options._mqttPort, 60);
  if (rc != MOSQ_ERR_SUCCESS)
  {
    std::cerr << "mqtt connect failed: " << mosqpp::strerror(rc) << std::endl;
    return false ;
  }

  _loopThread = std::thread(&Writer::networkLoop, this) ;

  return true ;
}

void Writer::networkLoop()
{
  // own loop instead of loop_start(): loop_forever() gives up for good on
  // some errors (e.g. a TLS connection closed without close_notify)
  int rc = MOSQ_ERR_SUCCESS ;

  while (!_loopStop)
  {
    if (rc == MOSQ_ERR_SUCCESS)
      rc = loop(1000, 1) ;
    if (rc == MOSQ_ERR_SUCCESS)
      continue ;

    if (shutdown)
    {
      // stop() sends 'offline' and disconnects, do not reconnect
      std::this_thread::sleep_for(std::chrono::milliseconds(100)) ;
      rc = MOSQ_ERR_SUCCESS ;
      continue ;
    }

    int delay = _reconnectDelay ;
    std::cerr << "mqtt: " << mosqpp::strerror(rc) << ", reconnecting in " << delay << " s" << std::endl ;
    for (int i = 0 ; (i < delay * 10) && !shutdown ; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100)) ;
    _reconnectDelay = std::min(delay * 2, 60) ;

    if (!shutdown)
      rc = reconnect() ;
  }
}

std::atomic<bool> shutdownWriter ;

void Writer::on_connect(int rc)
{
  if (rc != 0)
  {
    std::cerr << "mqtt connect refused: " << mosqpp::connack_string(rc) << std::endl ;
    return ;
  }

  if (options._mqttTopicStatus.size())
    publish(nullptr, options._mqttTopicStatus.c_str(), static_cast<int>(statusOnline.size()), statusOnline.c_str(), 1, true) ;

  if (_thread.joinable())
  {
    _thread.detach() ;
  }
  shutdownWriter = false;

  _thread = std::thread([this]()
  {
    std::vector<uint8_t> data ;

    _queueIts.clear() ;
    while (!shutdown)
    {
      if (!_queueIts.pop(data))
        continue ;

      if (shutdownWriter)
        return ;

      publish(nullptr, options._mqttTopicPacket.c_str(), static_cast<int>(data.size()), data.data(), 0, false);
    }
  }) ;

  if (rc == 0)
    _reconnectDelay = 1 ;
}

void Writer::on_disconnect(int rc)
{
  if (rc != 0)
    std::cerr << "mqtt disconnected: " << mosqpp::strerror(rc) << std::endl ;

  shutdownWriter = true ;
  _queueIts.push({}) ;
}

void Writer::stop()
{
  if (_thread.joinable())
    _thread.join() ;

  if (options._mqttTopicStatus.size())
  {
    publish(nullptr, options._mqttTopicStatus.c_str(), static_cast<int>(statusOffline.size()), statusOffline.c_str(), 1, true) ;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  disconnect() ;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  _loopStop = true ;
  if (_loopThread.joinable())
    _loopThread.join() ;
  mosqpp::lib_cleanup() ;
}