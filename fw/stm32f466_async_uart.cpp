// Copyright 2018 Josh Pieper, jjp@pobox.com.  All rights reserved.

#include "stm32f466_async_uart.h"

#include <tuple>

#include "mbed.h"
#include "serial_api_hal.h"

#include "error.h"
#include "irq_callback_table.h"

namespace {
IRQn_Type FindUartRxIrq(USART_TypeDef* uart) {
  switch(reinterpret_cast<uint32_t>(uart)) {
    case UART_1: return USART1_IRQn;
    case UART_2: return USART2_IRQn;
    case UART_3: return USART3_IRQn;
    case UART_4: return UART4_IRQn;
    case UART_5: return UART5_IRQn;
    case UART_6: return USART6_IRQn;
  }
  MBED_ASSERT(false);
  return {};
}
}

class Stm32F466AsyncUart::Impl : public RawSerial {
 public:
  Impl(EventQueue* event_queue, const Options& options)
      : RawSerial(options.tx, options.rx, options.baud_rate),
        event_queue_(event_queue) {
    // Just in case no one else has done it yet.
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();

    uart_ = [&]() {
      const auto uart_tx = static_cast<UARTName>(
          pinmap_peripheral(options.tx, PinMap_UART_TX));
      const auto uart_rx = static_cast<UARTName>(
        pinmap_peripheral(options.rx, PinMap_UART_RX));
      return reinterpret_cast<USART_TypeDef*>(pinmap_merge(uart_tx, uart_rx));
    }();
    MBED_ASSERT(uart_ != nullptr);
    uart_rx_irq_ = FindUartRxIrq(uart_);

    // TODO(josh.pieper): For now, we will hard-code which stream to
    // use when there are multiple options.  Perhaps later, the
    // Options we get passed in could provide a requirement if
    // necessary.
    std::tie(tx_dma_, rx_dma_) =
        MakeDma(static_cast<UARTName>(reinterpret_cast<int>(uart_)));

    // Roughly follow the procedure laid out in AN4031: Using the
    // STM32F2, STM32F4, and STM32F7 Series DMA Controller, section
    // 1.2

    // TODO(jpieper): This will only function if the DMA controller is
    // in a pristine state.  You could imagine asserting that is the
    // case, or even better, getting it into an appropriate state.  We
    // won't worry about it for now.

    // TODO(jpieper): Configure the FIFO to reduce the bus contention
    // involved with writing data.

    if (options.tx != NC) {
      tx_dma_.stream -> PAR = reinterpret_cast<uint32_t>(&(uart_->DR));
      tx_dma_.stream -> CR =
          tx_dma_.channel |
          DMA_SxCR_MINC |
          DMA_MEMORY_TO_PERIPH |
          DMA_SxCR_TCIE | DMA_SxCR_TEIE;

      tx_callback_ = IrqCallbackTable::MakeFunction([this]() {
          this->HandleTransmit();
        });
      NVIC_SetVector(tx_dma_.irq, reinterpret_cast<uint32_t>(tx_callback_.irq_function));
      NVIC_EnableIRQ(tx_dma_.irq);
    }

    if (options.rx != NC) {
      rx_dma_.stream -> PAR = reinterpret_cast<uint32_t>(&(uart_->DR));
      rx_dma_.stream -> CR =
          rx_dma_.channel | DMA_SxCR_MINC | DMA_PERIPH_TO_MEMORY |
          DMA_SxCR_TCIE | DMA_SxCR_TEIE;

      rx_callback_ = IrqCallbackTable::MakeFunction([this]() {
          this->HandleReceive();
        });
      NVIC_SetVector(rx_dma_.irq, reinterpret_cast<uint32_t>(rx_callback_.irq_function));
      NVIC_EnableIRQ(rx_dma_.irq);

      // Terminate DMA transactions when there is idle time on the bus.
      uart_->CR1 |= USART_CR1_IDLEIE;

      uart_callback_ = IrqCallbackTable::MakeFunction([this]() {
          this->HandleUart();
        });
      NVIC_SetVector(uart_rx_irq_, reinterpret_cast<uint32_t>(uart_callback_.irq_function));
      NVIC_EnableIRQ(uart_rx_irq_);
    }
  }

  void AsyncReadSome(const string_span& data, const SizeCallback& callback) {
    MBED_ASSERT(!current_read_callback_.valid());

    current_read_callback_ = callback;
    rx_size_ = data.size();

    // AN4031, 4.2: Clear all status registers.

    *rx_dma_.status_clear |= (
        rx_dma_.status_tcif |
        rx_dma_.status_htif |
        rx_dma_.status_teif |
        rx_dma_.status_dmeif |
        rx_dma_.status_feif);

    rx_dma_.stream->NDTR = data.size();
    rx_dma_.stream->M0AR = reinterpret_cast<uint32_t>(data.data());
    rx_dma_.stream->CR |= DMA_SxCR_EN;

    uart_  -> CR3 |= USART_CR3_DMAR;

    printf("AsyncReadSome size=%d\r\n", data.size());
  }

  void AsyncWriteSome(const string_view& data, const SizeCallback& callback) {
    MBED_ASSERT(!current_write_callback_.valid());

    current_write_callback_ = callback;
    tx_size_ = data.size();

    // AN4031, 4.2: Clear all status registers.

    *tx_dma_.status_clear |= (
        tx_dma_.status_tcif |
        tx_dma_.status_htif |
        tx_dma_.status_teif |
        tx_dma_.status_dmeif |
        tx_dma_.status_feif);

    tx_dma_.stream->NDTR = data.size();
    tx_dma_.stream->M0AR = reinterpret_cast<uint32_t>(data.data());
    tx_dma_.stream->CR |= DMA_SxCR_EN;

    uart_ -> CR3 |= USART_CR3_DMAT;
  }

  // INVOKED FROM INTERRUPT CONTEXT
  void HandleTransmit() {
    const size_t amount_sent = tx_size_ - tx_dma_.stream->NDTR;
    int error_code = 0;

    // The enable bit should be 0 at this point.
    MBED_ASSERT((tx_dma_.stream->CR & DMA_SxCR_EN) == 0);

    // Tell the UART to stop requesting DMA.
    uart_->CR3 &= ~(USART_CR3_DMAT);

    if (*tx_dma_.status_register & tx_dma_.status_teif) {
      // We've got an error, report it.
      *tx_dma_.status_clear |= tx_dma_.status_teif;
      error_code = kDmaStreamTransferError;
    } else if (*tx_dma_.status_register & tx_dma_.status_feif) {
      *tx_dma_.status_clear |= tx_dma_.status_feif;
      error_code = kDmaStreamFifoError;
    } else  if (*tx_dma_.status_register & tx_dma_.status_tcif) {
      // Transmit is complete.
      *tx_dma_.status_clear |= tx_dma_.status_tcif;
      error_code = 0;
    } else {
      MBED_ASSERT(false);
    }

    const int id = event_queue_->call(this, &Impl::EventHandleTransmit, error_code, amount_sent);
    MBED_ASSERT(id != 0);

    // TODO(jpieper): Verify that USART_CR3_DMAT gets cleared here on
    // its own even if we send back to back quickly.
  }

  void EventHandleTransmit(int error_code, size_t amount_sent) {
    const int id = event_queue_->call(current_write_callback_, error_code, amount_sent);
    MBED_ASSERT(id != 0);
    current_write_callback_ = {};
  }

  // INVOKED FROM INTERRUPT CONTEXT
  void HandleReceive() {
    const size_t amount_received = rx_size_ - rx_dma_.stream->NDTR;
    int error_code = 0;

    if (*rx_dma_.status_register & rx_dma_.status_teif) {
      *rx_dma_.status_clear |= rx_dma_.status_teif;
      const auto uart_sr = uart_->SR;
      if (uart_sr & USART_SR_ORE) {
        error_code = kUartOverrunError;
      } else if (uart_sr & USART_SR_FE) {
        error_code = kUartFramingError;
      } else if (uart_sr & USART_SR_NE) {
        error_code = kUartNoiseError;
      } else {
        error_code = kDmaStreamTransferError;
      }
    } else if (*rx_dma_.status_register & rx_dma_.status_feif) {
      *rx_dma_.status_clear |= rx_dma_.status_feif;
      error_code = kDmaStreamFifoError;
    } else if (*rx_dma_.status_register & rx_dma_.status_tcif) {
      *rx_dma_.status_clear |= rx_dma_.status_tcif;
      error_code = 0;
    } else {
      MBED_ASSERT(false);
    }

    uart_  -> CR3 &= ~(USART_CR3_DMAR);

    const int id = event_queue_->call(this, &Impl::EventHandleReceive, error_code, amount_received);
    MBED_ASSERT(id != 0);
  }

  void EventHandleReceive(int error_code, size_t amount_received) {
    const int id = event_queue_->call(current_read_callback_, error_code, amount_received);
    MBED_ASSERT(id != 0);
    current_read_callback_ = {};
  }

  // INVOKED FROM INTERRUPT CONTEXT
  void HandleUart() {
    if (uart_->SR && USART_FLAG_IDLE) {
      // Clear the IDLE flag by reading status register, then data register.
      volatile uint32_t tmp;
      tmp = uart_->SR;
      tmp = uart_->DR;
      (void)tmp;

      // Disable any outstanding DMA transfers.
      rx_dma_.stream->CR &= ~DMA_SxCR_EN;
    }
  }

  struct Dma {
    DMA_Stream_TypeDef* stream;
    uint32_t channel;
    volatile uint32_t* status_clear;
    volatile uint32_t* status_register;
    uint32_t status_tcif;
    uint32_t status_htif;
    uint32_t status_teif;
    uint32_t status_dmeif;
    uint32_t status_feif;
    IRQn_Type irq;
  };

#define MAKE_UART(DmaNumber, StreamNumber, ChannelNumber, StatusRegister) \
  Dma {                                                                 \
    DmaNumber ## _Stream ## StreamNumber,                               \
        (ChannelNumber) << DMA_SxCR_CHSEL_Pos,                          \
        & ( DmaNumber -> StatusRegister ## FCR ),                       \
        & ( DmaNumber -> StatusRegister ## SR ),                        \
        DMA_ ## StatusRegister ## SR_TCIF ## StreamNumber,                \
        DMA_ ## StatusRegister ## SR_HTIF ## StreamNumber,                \
        DMA_ ## StatusRegister ## SR_TEIF ## StreamNumber,                \
        DMA_ ## StatusRegister ## SR_DMEIF ## StreamNumber,               \
        DMA_ ## StatusRegister ## SR_FEIF ## StreamNumber,                \
        DmaNumber ## _Stream ## StreamNumber ## _IRQn,                  \
        }

  std::pair<Dma, Dma> MakeDma(UARTName uart) {
    switch (uart) {
      case UART_1:
        return { MAKE_UART(DMA2, 7, 4, HI), MAKE_UART(DMA2, 2, 4, LI), };
      case UART_2:
        return { MAKE_UART(DMA1, 6, 4, HI), MAKE_UART(DMA1, 5, 4, HI), };
      case UART_3:
        return { MAKE_UART(DMA1, 3, 4, LI), MAKE_UART(DMA1, 1, 4, LI), };
      case UART_4:
        return { MAKE_UART(DMA1, 4, 4, HI), MAKE_UART(DMA1, 2, 4, LI), };
      case UART_5:
        return { MAKE_UART(DMA1, 7, 4, HI), MAKE_UART(DMA1, 0, 4, LI), };
      case UART_6:
        return { MAKE_UART(DMA2, 6, 5, HI), MAKE_UART(DMA2, 1, 5, LI), };
    }
    MBED_ASSERT(false);
    return {};
  }

#undef MAKE_UART

  events::EventQueue* const event_queue_;
  USART_TypeDef* uart_ = nullptr;
  IRQn_Type uart_rx_irq_ = {};

  Dma tx_dma_;
  Dma rx_dma_;

  IrqCallbackTable::Callback tx_callback_;
  IrqCallbackTable::Callback rx_callback_;
  IrqCallbackTable::Callback uart_callback_;

  SizeCallback current_read_callback_;
  size_t rx_size_ = 0;

  SizeCallback current_write_callback_;
  size_t tx_size_ = 0;
};

Stm32F466AsyncUart::Stm32F466AsyncUart(EventQueue* event_queue, const Options& options)
    : impl_(event_queue, options) {}
Stm32F466AsyncUart::~Stm32F466AsyncUart() {}

void Stm32F466AsyncUart::AsyncReadSome(const string_span& data,
                                       const SizeCallback& callback) {
  impl_->AsyncReadSome(data, callback);
}

void Stm32F466AsyncUart::AsyncWriteSome(const string_view& data,
                                        const SizeCallback& callback) {
  impl_->AsyncWriteSome(data, callback);
}