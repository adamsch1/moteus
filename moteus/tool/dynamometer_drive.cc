// Copyright 2020 Josh Pieper, jjp@pobox.com.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/// @file
///
/// Drive a dynamometer test fixture.

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/signals2/signal.hpp>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include "mjlib/base/clipp_archive.h"
#include "mjlib/base/clipp.h"
#include "mjlib/base/fail.h"
#include "mjlib/base/visitor.h"

#include "mjlib/io/now.h"
#include "mjlib/io/selector.h"

#include "mjlib/multiplex/asio_client.h"
#include "mjlib/multiplex/stream_asio_client_builder.h"

#include "mjlib/telemetry/binary_schema_parser.h"
#include "mjlib/telemetry/binary_write_archive.h"
#include "mjlib/telemetry/file_writer.h"
#include "mjlib/telemetry/mapped_binary_reader.h"

#include "moteus/tool/line_reader.h"
#include "moteus/tool/run_for.h"

namespace base = mjlib::base;
namespace io = mjlib::io;
namespace mp = mjlib::multiplex;

namespace {
/// This is a minimal copy of moteus::BldcServo::Status used to
/// monitor the servos in real-time.
struct ServoStats {
  enum Mode {
    kStopped = 0,
    kFault =1 ,
    kPwm = 5,
    kVoltage = 6,
    kVoltageFoc = 7,
    kVoltageDq = 8,
    kCurrent = 9,
    kPosition = 10,
    kPositionTimeout = 11,
    kZeroVelocity = 12,
  };
  Mode mode = kStopped;

  enum Fault {
    kNone = 0,
  };
  Fault fault = kNone;
  float torque_Nm = 0.0f;

  template <typename Archive>
  void Serialize(Archive* a) {
    a->Visit(MJ_NVP(mode));
    a->Visit(MJ_NVP(fault));
    a->Visit(MJ_NVP(torque_Nm));
  }
};
}  // namespace

namespace mjlib {
namespace base {
template <>
struct IsEnum<::ServoStats::Mode> {
  static constexpr bool value = true;

  using M = ::ServoStats::Mode;
  static std::array<std::pair<M, const char*>, 1> map() {
    return { {
        { M::kStopped, "stopped" },
            }};
  }
};

template <>
struct IsEnum<::ServoStats::Fault> {
  static constexpr bool value = true;

  using M = ::ServoStats::Fault;
  static std::array<std::pair<M, const char*>, 1> map() {
    return { {
        { M::kNone, "none" },
            }};
  }
};
}  // namespace base
}  // namespace mjlib

namespace moteus {
namespace tool {
namespace {

constexpr int kDebugTunnel = 1;

struct Options {
  int fixture_id = 32;
  int dut_id = 1;
  std::string torque_transducer;

  bool verbose = false;
  std::string log;
  double max_test_time_s = 1200.0;

  double max_torque_Nm = 0.5;


  // The different cycles we can do and their options.
  bool static_torque_ripple = false;
  double static_torque_ripple_speed = 0.01;

  bool pwm_cycle_overrun = false;

  template <typename Archive>
  void Serialize(Archive* a) {
    a->Visit(MJ_NVP(fixture_id));
    a->Visit(MJ_NVP(dut_id));
    a->Visit(MJ_NVP(torque_transducer));
    a->Visit(MJ_NVP(verbose));
    a->Visit(MJ_NVP(log));
    a->Visit(MJ_NVP(max_test_time_s));
    a->Visit(MJ_NVP(max_torque_Nm));

    a->Visit(MJ_NVP(static_torque_ripple));
    a->Visit(MJ_NVP(static_torque_ripple_speed));

    a->Visit(MJ_NVP(pwm_cycle_overrun));
  }
};

struct TorqueTransducer {
  boost::posix_time::ptime timestamp;

  uint32_t time_code = 0;
  double torque_Nm = 0.0;
  double temperature_C = 0.0;

  template <typename Archive>
  void Serialize(Archive* a) {
    a->Visit(MJ_NVP(timestamp));
    a->Visit(MJ_NVP(time_code));
    a->Visit(MJ_NVP(torque_Nm));
    a->Visit(MJ_NVP(temperature_C));
  }
};

void ExceptionRethrower(std::exception_ptr ptr) {
  if (ptr) {
    std::rethrow_exception(ptr);
  }
}

class LogRegistrar {
 public:
  LogRegistrar(mjlib::telemetry::FileWriter* file_writer)
      : file_writer_(file_writer) {}

  template <typename T>
  void Register(const std::string& name,
                boost::signals2::signal<void (const T*)>* signal) {
    const auto identifier = file_writer_->AllocateIdentifier(name);
    file_writer_->WriteSchema(
        identifier,
        mjlib::telemetry::BinarySchemaArchive::template schema<T>());
    signal->connect(std::bind(&LogRegistrar::HandleData<T>,
                              this, identifier, std::placeholders::_1));
  }

  template <typename T>
  void HandleData(mjlib::telemetry::FileWriter::Identifier identifier,
                  const T* data) {
    if (!file_writer_->IsOpen()) { return; }

    auto buffer = file_writer_->GetBuffer();
    mjlib::telemetry::BinaryWriteArchive(*buffer).Accept(data);
    file_writer_->WriteData({}, identifier, std::move(buffer));
  }

 private:
  mjlib::telemetry::FileWriter* const file_writer_;
};

std::string StringValue(const std::string& line_in) {
  const auto line = boost::trim_copy(line_in);
  std::size_t pos = line.find(' ');
  if (pos == std::string::npos) { return ""; }
  return line.substr(pos + 1);
}

class ServoStatsReader {
 public:
  ServoStatsReader(const std::string& schema) : schema_(schema) {}

  ServoStats Read(const std::string& data) {
    return reader_.Read(data);
  }

 private:
  const std::string schema_;
  mjlib::telemetry::BinarySchemaParser parser_{schema_, "servo_stats"};
  mjlib::telemetry::MappedBinaryReader<ServoStats> reader_{&parser_};
};

class Controller {
 public:
  Controller(const std::string& log_prefix,
             mjlib::telemetry::FileWriter* file_writer,
             mjlib::io::SharedStream stream,
             const Options& options)
      : log_prefix_(log_prefix),
        stream_(stream),
        file_writer_(file_writer),
        options_(options) {}

  std::string stats() const {
    return fmt::format("{:2d} {:6.3f}", servo_stats_.mode, servo_stats_.torque_Nm);
  }

  const ServoStats& servo_stats() const {
    return servo_stats_;
  }

  boost::asio::awaitable<void> OKReceived() {
    auto operation = [this](io::ErrorCallback callback) {
      BOOST_ASSERT(!ok_callback_);
      ok_callback_ = std::move(callback);
    };

    co_await async_initiate<
      decltype(boost::asio::use_awaitable),
      void(boost::system::error_code)>(operation, boost::asio::use_awaitable);
  }

  boost::asio::awaitable<void> SomethingReceived() {
    auto operation = [this](io::ErrorCallback callback) {
      BOOST_ASSERT(!received_callback_);
      received_callback_ = std::move(callback);
    };

    co_await async_initiate<
      decltype(boost::asio::use_awaitable),
      void(boost::system::error_code)>(operation, boost::asio::use_awaitable);
  }

  boost::asio::awaitable<void> Start() {
    co_await WriteMessage("tel stop\nd stop\n");

    char buf[4096] = {};

    // Discard anything we receive for some amount of time.
    co_await RunFor(
        stream_->get_executor(),
        *stream_,
        [&]() -> boost::asio::awaitable<bool> {
          boost::system::error_code ec;
          co_await boost::asio::async_read(
              *stream_,
              boost::asio::buffer(buf),
              boost::asio::redirect_error(boost::asio::use_awaitable, ec));
          // We just ignore ec.
          co_return false;
        },
        mjlib::io::Now(stream_->get_executor().context()) +
        boost::posix_time::milliseconds(300));

    boost::asio::co_spawn(
        stream_->get_executor(),
        std::bind(&Controller::Run, this),
        ExceptionRethrower);

    std::vector<std::string> names = {
      "servo_stats",
      "servo_control",
      "servo_cmd",
      "git",
    };

    for (const auto& name : names) {
      co_await WriteMessage(fmt::format("tel schema {}", name));
      while (true) {
        co_await SomethingReceived();
        if (log_ids_.count(name) != 0) { break; }
      }

      co_await Command(fmt::format("tel rate {} 50", name));

      // Now wait for this to have data.
      while (true) {
        co_await SomethingReceived();
        if (log_data_.count(name) != 0) { break; }
      }
    }

    co_return;
  }

  boost::asio::awaitable<void> Command(const std::string& message) {
    co_await WriteMessage(message);
    co_await OKReceived();
    co_return;
  }

  boost::asio::awaitable<void> WriteMessage(const std::string& message) {
    if (options_.verbose) {
      fmt::print("> {}\n", message);
    }
    co_await boost::asio::async_write(
        *stream_,
        boost::asio::buffer(message + "\n"),
        boost::asio::use_awaitable);
    co_return;
  }

  boost::asio::awaitable<void> Run() {
    // Read from the stream, barfing on errors, and logging any binary
    // data that comes our way.
    while (true) {
      const auto maybe_line = co_await reader_.ReadLine();

      if (!maybe_line) {
        // We had a timeout, this should only happen here on error.
        throw mjlib::base::system_error::einval("Timeout");
      }
      const auto line = *maybe_line;
      if (boost::starts_with(line, "OK")) {
        if (ok_callback_) {
          boost::asio::post(
              stream_->get_executor(),
              std::bind(std::move(ok_callback_), mjlib::base::error_code()));
        }
        // We ignore these.
      } else if (boost::starts_with(line, "ERR")) {
        // This causes us to terminate immediately.
        throw base::system_error::einval(line);
      } else if (boost::starts_with(line, "schema ")) {
        const auto name = StringValue(line);

        const auto ident = file_writer_->AllocateIdentifier(log_prefix_ + name);
        log_ids_[name] = ident;
        const std::string schema = co_await reader_.ReadBinaryBlob();
        file_writer_->WriteSchema(ident, schema);

        if (name == "servo_stats") {
          servo_stats_reader_.emplace(schema);
        }
      } else if (boost::starts_with(line, "emit ")) {
        const auto name = StringValue(line);
        if (log_ids_.count(name)) {
          const auto ident = log_ids_.at(name);
          log_data_.insert(name);
          const auto data = co_await reader_.ReadBinaryBlob();
          file_writer_->WriteData({}, ident, data);
          if (name == "servo_stats") {
            HandleServoStats(servo_stats_reader_->Read(data));
          }
        }
      } else {
        fmt::print("Ignoring unknown line: {}\n", line);
      }

      if (received_callback_) {
        boost::asio::post(
            stream_->get_executor(),
            std::bind(std::move(received_callback_), mjlib::base::error_code()));
      }
    }

    co_return;
  }

  mjlib::io::AsyncStream& stream() { return *stream_; }

 private:
  void HandleServoStats(const ServoStats& servo_stats) {
    servo_stats_ = servo_stats;
    // By default, we throw an exception for any fault.
    if (servo_stats_.mode == ServoStats::kFault) {
      throw mjlib::base::system_error::einval(
          fmt::format("Fault: {} {}", log_prefix_,
                      static_cast<int>(servo_stats.fault)));
    }
  }

  const std::string log_prefix_;
  mjlib::io::SharedStream stream_;
  mjlib::telemetry::FileWriter* const file_writer_;
  const Options options_;

  LineReader reader_{stream_.get(), [&]() {
      LineReader::Options o;
      o.set_verbose(options_.verbose);
      return o;
    }()};

  std::map<std::string, uint64_t> log_ids_;
  std::set<std::string> log_data_;

  io::ErrorCallback received_callback_;
  io::ErrorCallback ok_callback_;

  std::optional<ServoStatsReader> servo_stats_reader_;
  ServoStats servo_stats_;
};

class Application {
 public:
  Application(boost::asio::io_context& context,
              io::Selector<mp::AsioClient>* selector,
              const Options& options)
      : executor_(context.get_executor()),
        client_selector_(selector),
        options_(options),
        file_writer_() {}

  void Start() {
    if (!options_.log.empty()) {
      file_writer_.Open(options_.log);
    }
    client_selector_->AsyncStart(
        std::bind(&Application::HandleClient, this, std::placeholders::_1));
  }

  void HandleClient(const base::error_code& ec) {
    base::FailIf(ec);

    client_ = client_selector_->selected();

    boost::asio::co_spawn(
        executor_,
        std::bind(&Application::Task, this),
        [](std::exception_ptr ptr) {
          if (ptr) {
            std::rethrow_exception(ptr);
          }
          std::exit(0);
        });
  }

  boost::asio::awaitable<void> Task() {
    co_await Init();

    const auto expiration =
        mjlib::io::Now(executor_.context()) +
        mjlib::base::ConvertSecondsToDuration(options_.max_test_time_s);

    co_await RunFor(
        executor_,
        fixture_->stream(),
        [&]() -> boost::asio::awaitable<bool> {
          std::exception_ptr eptr;
          try {
            co_await RunTestCycle();
          } catch (mjlib::base::system_error& se) {
            eptr = std::current_exception();
          }
          co_await Stop();
          if (eptr) {
            std::rethrow_exception(eptr);
          }
          co_return false;
        },
        expiration);

    co_return;
  }

  boost::asio::awaitable<void> RunTestCycle() {
    if (options_.static_torque_ripple) {
      co_await RunStaticTorqueRipple();
    } else if (options_.pwm_cycle_overrun) {
      co_await RunPwmCycleOverrun();
    } else {
      fmt::print("No cycle selected\n");
    }
    co_return;
  }

  boost::asio::awaitable<void> Stop() {
    // Make sure everything is stopped.  We use WriteMessage here,
    // since it doesn't rely on the read loops operating.

    co_await dut_->WriteMessage("d stop");
    co_await fixture_->WriteMessage("d stop");
    co_await dut_->WriteMessage("tel stop");
    co_await fixture_->WriteMessage("tel stop");

    // Since we can't use the read loops, just wait a bit to make sure
    // those commands go out.
    co_await Sleep(1.0);

    co_return;
  }

  boost::asio::awaitable<void> RunStaticTorqueRipple() {
    co_await fixture_->Command(
        "conf set servo.pid_position.ki 200.0");
    co_await fixture_->Command(
        "conf set servo.pid_position.kp 10.0");
    co_await fixture_->Command(
        fmt::format("conf set servo.pid_position.ilimit {}",
                    std::max(0.3, options_.max_torque_Nm)));
    co_await fixture_->Command("conf set servopos.position_min nan");
    co_await fixture_->Command("conf set servopos.position_max nan");
    co_await dut_->Command("conf set servopos.position_min nan");
    co_await dut_->Command("conf set servopos.position_max nan");

    // Then start commanding the different torques on the dut servo,
    // each for a bit more than one revolution.
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    for (double test_torque : {kNaN, 0.0, 0.05, -0.05, 0.1, -0.1, 0.2, -0.2}) {
      fmt::print("\nTesting torque: {}\n", test_torque);

      co_await fixture_->Command("d rezero");
      co_await dut_->Command("d rezero");

      // Start the fixture sweeping.
      co_await fixture_->Command(
          fmt::format("d pos nan {} {}",
                      options_.static_torque_ripple_speed,
                      options_.max_torque_Nm));


      if (std::isfinite(test_torque)) {
        co_await dut_->Command(fmt::format("d pos nan 0 {} p0 d0 f{}",
                                           std::abs(test_torque), test_torque));
      } else {
        co_await dut_->Command(fmt::format("d stop"));
      }

      // Wait for the time required by one full cycle plus a bit while
      // printing our status.
      StatusPrinter status_printer(
          this, fmt::format("STAT_TOR({})", test_torque));

      co_await Sleep(1.2 / options_.static_torque_ripple_speed);

      co_await dut_->Command("d stop");
      co_await fixture_->Command("d stop");
    }
  }

  boost::asio::awaitable<void> RunPwmCycleOverrun() {
    co_await fixture_->Command("d stop");
    co_await dut_->Command("conf set servopos.position_min -3.0");
    co_await dut_->Command("conf set servopos.position_max 3.0");

    constexpr int kPulseCount = 4;
    constexpr double kPulseTorque = 1.5;

    for (int i = 0; i < kPulseCount; i++) {
      // Now, what we'll do is command a moderate constant torque with
      // no load presented by the fixture.  That should result in the
      // current loop maxing out, which if the PWM cycle isn't
      // configured correctly will fault.  If it is configured
      // correctly, we'll just zoom past the position limit and stop.
      fmt::print("About to pulse {}/{}\n", i + 1, kPulseCount);
      co_await Sleep(1.0);

      co_await dut_->Command("d rezero");
      co_await Sleep(0.1);

      co_await dut_->Command(
          fmt::format("d pos nan 0 {} p0 d0 f{}",
                      kPulseTorque,
                      kPulseTorque * ((i % 2) == 0 ? 1.0 : -1.0)));

      co_await Sleep(4.0);

      // Verify that we have not faulted.
      if (dut_->servo_stats().mode != ServoStats::kPosition) {
        throw mjlib::base::system_error::einval("DUT no longer in position mode");
      }
    }

    fmt::print("Test passed\n");

    co_return;
  }

  boost::asio::awaitable<void> Sleep(double seconds) {
    boost::asio::deadline_timer timer(executor_);
    timer.expires_from_now(mjlib::base::ConvertSecondsToDuration(seconds));
    co_await timer.async_wait(boost::asio::use_awaitable);
    co_return;
  }

  boost::asio::awaitable<void> Init() {
    // First, connect to our two devices.
    fixture_.emplace(
        "fixture_",
        &file_writer_,
        client_->MakeTunnel(options_.fixture_id, kDebugTunnel, {}),
        options_);
    dut_.emplace(
        "dut_",
        &file_writer_,
        client_->MakeTunnel(options_.dut_id, kDebugTunnel, {}),
        options_);

    const auto expiration =
        mjlib::io::Now(executor_.context()) +
        boost::posix_time::seconds(3);

    co_await RunFor(
        executor_,
        fixture_->stream(),
        [&]() -> boost::asio::awaitable<bool> {
          co_await fixture_->Start();
          co_await dut_->Start();
          co_return false;
        },
        expiration);

    // Then the torque transducer.
    log_registrar_.Register("torque", &torque_signal_);
    boost::asio::co_spawn(
        executor_,
        std::bind(&Application::ReadTorque, this),
        ExceptionRethrower);

    // Then the current sense.

    // Give things a little bit to wait.
    boost::asio::deadline_timer timer(executor_);
    timer.expires_from_now(boost::posix_time::milliseconds(500));
    co_await timer.async_wait(boost::asio::use_awaitable);

    fmt::print("Initialization complete\n");

    co_return;
  }

  boost::asio::awaitable<void> ReadTorque() {
    boost::asio::serial_port port(executor_, options_.torque_transducer);

    boost::asio::streambuf streambuf;
    std::istream istr(&streambuf);
    while (true) {
      co_await boost::asio::async_read_until(
          port,
          streambuf,
          "\n",
          boost::asio::use_awaitable);

      std::string line;
      std::getline(istr, line);
      EmitTorque(line);
    }
  }

  void EmitTorque(const std::string& line) {
    std::vector<std::string> fields;
    boost::split(fields, line, boost::is_any_of(","));
    if (fields.size() < 4) { return; }
    TorqueTransducer data;
    try {
      data.timestamp = mjlib::io::Now(executor_.context());
      data.time_code = std::stol(fields.at(0));
      data.torque_Nm = std::stod(fields.at(1)) - torque_tare_;
      data.temperature_C = std::stod(fields.at(3));

      // We skip the first N samples to tare on startup.
      constexpr double kNumTareSamples = 4;
      if (torque_tare_count_ < kNumTareSamples) {
        torque_tare_total_ += data.torque_Nm;
        torque_tare_count_++;
        if (torque_tare_count_ >= kNumTareSamples) {
          torque_tare_ = torque_tare_total_ / torque_tare_count_;
        }
      }
    } catch (std::invalid_argument& e) {
      fmt::print("Ignoring torque data: '{}': {}", line, e.what());
      // Ignore.
      return;
    }
    torque_signal_(&data);
  }

  class StatusPrinter {
   public:
    StatusPrinter(Application* parent, const std::string& header) {
      boost::asio::co_spawn(
          parent->executor_,
          std::bind(&StatusPrinter::Run, parent, header, done_),
          ExceptionRethrower);
    }

    ~StatusPrinter() {
      *done_ = true;
      fmt::print("\n");
    }

    static boost::asio::awaitable<void> Run(
        Application* app, const std::string& header,
        std::shared_ptr<bool> done) {
      const auto start = mjlib::io::Now(app->executor_.context());
      const auto elapsed = [&]() {
        const auto now = mjlib::io::Now(app->executor_.context());
        const auto delta = now - start;
        return fmt::format("{:02d}:{:02d}:{:02d}",
                           delta.hours(), delta.minutes(), delta.seconds());
      };

      while (!*done) {
        fmt::print("{} {} fixture=({})  dut=({})\r",
                   elapsed(), header, app->fixture_->stats(), app->dut_->stats());
        ::fflush(stdout);

        boost::asio::deadline_timer timer(app->executor_);
        timer.expires_from_now(boost::posix_time::seconds(1));
        co_await timer.async_wait(boost::asio::use_awaitable);
      }
    }

    std::shared_ptr<bool> done_ = std::make_shared<bool>(false);
  };

  boost::asio::any_io_executor executor_;
  boost::asio::executor_work_guard<
    boost::asio::any_io_executor> guard_{executor_};
  io::Selector<mp::AsioClient>* const client_selector_;
  const Options& options_;

  mp::AsioClient* client_ = nullptr;

  mjlib::telemetry::FileWriter file_writer_;
  LogRegistrar log_registrar_{&file_writer_};

  std::optional<Controller> fixture_;
  std::optional<Controller> dut_;

  boost::signals2::signal<void (const TorqueTransducer*)> torque_signal_;

  double torque_tare_total_ = 0.0;
  double torque_tare_count_ = 0;
  double torque_tare_ = 0.0;
};

int do_main(int argc, char** argv) {
  boost::asio::io_context context;

  io::Selector<mp::AsioClient> default_client_selector{
    context.get_executor(), "client_type"};

  mp::StreamAsioClientBuilder::Options default_stream_options;
  default_stream_options.stream.type = io::StreamFactory::Type::kSerial;
  default_stream_options.stream.serial_port = "/dev/fdcanusb";
  // If the baud rate does matter, 3mbit is a good one.
  default_stream_options.stream.serial_baud = 3000000;

  default_client_selector.Register<mp::StreamAsioClientBuilder>(
      "stream", default_stream_options);
  default_client_selector.set_default("stream");

  Options options;

  auto group = mjlib::base::ClippArchive().Accept(&options).group();
  group.merge(clipp::with_prefix(
                  "client.", default_client_selector.program_options()));

  mjlib::base::ClippParse(argc, argv, group);

  Application application{context, &default_client_selector, options};
  application.Start();
  try {
    context.run();
  } catch (std::runtime_error& e) {
    fmt::print(stderr, "Error: {}\n", e.what());
    return 1;
  }

  return 0;
}

}
}
}

int main(int argc, char** argv) {
  return ::moteus::tool::do_main(argc, argv);
}
