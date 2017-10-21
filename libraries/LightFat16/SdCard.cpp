/* Arduino FAT16 Library
 * Copyright (C) 2008 by William Greiman
 *
 * This file is part of the Arduino FAT16 Library
 *
 * This Library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with the Arduino Fat16 Library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */
#include <Fat16Config.h>
#include <SdCard.h>
#include <SPI.h>
#include <VarioSettings.h>

//------------------------------------------------------------------------------
#if 0//////////////////////////////////////////////////////////////////////////////////////////////////
// r1 status values
uint8_t const R1_READY_STATE = 0;
uint8_t const R1_IDLE_STATE  = 1;
// start data token for read or write
uint8_t const DATA_START_BLOCK = 0XFE;
// data response tokens for write block
uint8_t const DATA_RES_MASK        = 0X1F;
uint8_t const DATA_RES_ACCEPTED    = 0X05;
uint8_t const DATA_RES_CRC_ERROR   = 0X0B;
uint8_t const DATA_RES_WRITE_ERROR = 0X0D;
#endif////////////////////////////////////////////////////////////////////////////////////////////////////
//
// stop compiler from inlining where speed optimization is not required
#define STATIC_NOINLINE static __attribute__((noinline))

//------------------------------------------------------------------------------
// wait for card to go not busy
// return false if timeout
STATIC_NOINLINE bool waitForToken(uint8_t token, uint16_t timeoutMillis) {
  uint16_t t0 = millis();
  while (SDCARD_SPI_INTERFACE.transfer(0xff) != token) {
    if (((uint16_t)millis() - t0) > timeoutMillis) return false;
  }
  return true;
}
//==============================================================================
// SdCard member functions
//------------------------------------------------------------------------------
uint8_t SdCard::cardCommand(uint8_t cmd, uint32_t arg) {
  // select card
  startSPI();
  
  // wait if busy
  waitForToken(0xff, SD_WRITE_TIMEOUT);

  // send command
  SDCARD_SPI_INTERFACE.transfer(cmd | 0x40);

  // send argument
  uint8_t *pa = reinterpret_cast<uint8_t *>(&arg);
  for (int8_t i = 3; i >= 0; i--) {
    SDCARD_SPI_INTERFACE.transfer(pa[i]);
  }
  
  // send CRC - correct for CMD0 with arg zero or CMD8 with arg 0X1AA
  SDCARD_SPI_INTERFACE.transfer(cmd == CMD0 ? 0X95 : 0X87);

  // skip stuff byte for stop read
  if (cmd == CMD12) {
    SDCARD_SPI_INTERFACE.transfer(0xff);
  }

  // wait for response
  uint8_t status;
  for (uint8_t i = 0; ((status = SDCARD_SPI_INTERFACE.transfer(0xff)) & 0X80) && i != 0XFF; i++) {
  }
  return status;
}
//------------------------------------------------------------------------------
uint8_t SdCard::cardAcmd(uint8_t cmd, uint32_t arg) {
  cardCommand(CMD55, 0);
  return cardCommand(cmd, arg);
}


//------------------------------------------------------------------------------
/**
 * Initialize a SD flash memory card.
 *
 * \param[in] chipSelect SD chip select pin number.
 * \param[in] sckDivisor SPI clock divisor.
 *
 * \return The value one, true, is returned for success and
 * the value zero, false, is returned for failure.
 *
 */
void SdCard::enableSPI(void) {

  /* prevent enabling hardware SS pin */ 
  pinMode(SS, OUTPUT);

  /* set real CS line */
  digitalWrite(chipSelectPin, HIGH);
  pinMode(chipSelectPin, OUTPUT);
  
  /* set SPI */ 
  SDCARD_SPI_INTERFACE.begin();
}

void SdCard::startSPI(void) {
  if( ! spiStarted ) {
    SDCARD_SPI_INTERFACE.beginTransaction(SD_SPI_SETTINGS);
    digitalWrite(chipSelectPin, LOW);
    spiStarted = true;
  }
}

void SdCard::stopSPI(void) {
  if( spiStarted ) {
    digitalWrite(chipSelectPin, HIGH);
    SDCARD_SPI_INTERFACE.transfer(0xff);
    SDCARD_SPI_INTERFACE.endTransaction();
    spiStarted = false;
  }
}


bool SdCard::begin(void) {

  uint8_t status;

  // 16-bit init start time allows over a minute
  unsigned t0 = (unsigned)millis();
  uint32_t arg;

  // initialize SPI bus and chip select pin.
  enableSPI();

  // set SCK rate for initialization commands.
  SDCARD_SPI_INTERFACE.beginTransaction(SD_SPI_INIT_SETTINGS);
  digitalWrite(chipSelectPin, LOW);
  
  // must supply min of 74 clock cycles with CS high.
  for (uint8_t i = 0; i < 10; i++) {
    SDCARD_SPI_INTERFACE.transfer(0xff);
  }
  // command to go idle in SPI mode
  while (cardCommand(CMD0, 0) != R1_IDLE_STATE) {
    if (((unsigned)millis() - t0) > SD_INIT_TIMEOUT) {
      goto fail;
    }
  }

  // check SD version
  while (1) {
    if (cardCommand(CMD8, 0x1AA) == (R1_ILLEGAL_COMMAND | R1_IDLE_STATE)) {
      cardType = CARD_TYPE_SDV1;
      break;
    }
    for (uint8_t i = 0; i < 4; i++) {
      status = SDCARD_SPI_INTERFACE.transfer(0xff);
    }
    if (status == 0XAA) {
      cardType = CARD_TYPE_SDV2;
      break;
    }
    if (((unsigned)millis() - t0) > SD_INIT_TIMEOUT) {
      goto fail;
    }
  }

    
  // initialize card and send host supports SDHC if SD2
  arg = cardType == CARD_TYPE_SDV2 ? 0X40000000 : 0;
    
  while (cardAcmd(ACMD41, arg) != R1_READY_STATE) {
    // check for timeout
    if (((unsigned)millis() - t0) > SD_INIT_TIMEOUT) {
      goto fail;
    }
  }
    
  // if SD2 read OCR register to check for SDHC card
  if (cardType == CARD_TYPE_SDV2) {
    if (cardCommand(CMD58, 0)) {
      goto fail;
    }
    if ((SDCARD_SPI_INTERFACE.transfer(0xff) & 0XC0) == 0XC0) {
      cardType = CARD_TYPE_SDHC;
    }
    // Discard rest of ocr - contains allowed voltage range.
    for (uint8_t i = 0; i < 3; i++) {
      SDCARD_SPI_INTERFACE.transfer(0xff);
    }
  }

  /* reset SPI clock */
  stopSPI();
  return true;

 fail:
  stopSPI();
  return false;
}

//------------------------------------------------------------------------------
/**
 * Reads a 512 byte block from a storage device.
 *
 * \param[in] blockNumber Logical block to be read.
 * \param[out] dst Pointer to the location that will receive the data.
 * \return The value one, true, is returned for success and
 * the value zero, false, is returned for failure.
 */
bool SdCard::readBlock(uint32_t blockNumber, uint8_t* dst) {

  uint8_t status;
  unsigned t0;
  
  /* get block number */
  if (cardType != CARD_TYPE_SDHC) {
    blockNumber <<= 9;
  }
  if (cardCommand(CMD17, blockNumber)) {
    goto fail;
  }
  
  /********/
  /* read */
  /********/
  // wait for start block token
  t0 = millis();
  while ((status = SDCARD_SPI_INTERFACE.transfer(0xff)) == 0XFF) {
    if (((unsigned)millis() - t0) > SD_READ_TIMEOUT) {
      goto fail;
    }
  }
  if (status != DATA_START_BLOCK) {
    goto fail;
  }
  // transfer data
  for (size_t i = 0; i < 512; i++) {
    dst[i] = SDCARD_SPI_INTERFACE.transfer(0XFF);
  }
  
  // discard crc
  SDCARD_SPI_INTERFACE.transfer(0XFF);
  SDCARD_SPI_INTERFACE.transfer(0XFF);

  // ok
  stopSPI();
  return true;

fail:
  stopSPI();
  return false;
}
//------------------------------------------------------------------------------
/**
 * Writes a 512 byte block to a storage device.
 *
 * \param[in] blockNumber Logical block to be written.
 * \param[in] src Pointer to the location of the data to be written.
 * \return The value one, true, is returned for success and
 * the value zero, false, is returned for failure.
 */
bool SdCard::writeBlock(uint32_t blockNumber, const uint8_t* src) {
  uint8_t status;
  
  /* set block number */
  if (cardType != CARD_TYPE_SDHC) {
    blockNumber <<= 9;
  }
  if (cardCommand(CMD24, blockNumber)) {
    goto fail;
  }

  /*********/
  /* write */
  /*********/
  SDCARD_SPI_INTERFACE.transfer(0xff);
  SDCARD_SPI_INTERFACE.transfer(0xff);
  
  SDCARD_SPI_INTERFACE.transfer(DATA_START_BLOCK);
  for (size_t i = 0; i < 512; i++) {
    SDCARD_SPI_INTERFACE.transfer(src[i]);
  }
  SDCARD_SPI_INTERFACE.transfer(0xff);
  SDCARD_SPI_INTERFACE.transfer(0xff);
  
  status = SDCARD_SPI_INTERFACE.transfer(0xff);
  if ((status & DATA_RES_MASK) != DATA_RES_ACCEPTED) {
    goto fail;
  }

  // ok
  stopSPI();
  return true;
  
 fail:
  stopSPI();
  return false;
}