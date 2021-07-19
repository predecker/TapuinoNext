#include "TapRecorder.h"
#include "Lang.h"
#include <Arduino.h>

using namespace std;

using namespace TapuinoNext;

TapRecorder::TapRecorder(UtilityCollection* utilityCollection, uint32_t bufferSize) : TapBase(utilityCollection, bufferSize)
{
}

TapRecorder::~TapRecorder()
{
}

void TapRecorder::WriteByte(uint8_t value)
{
    if (bufferPos == bufferSwitchPos)
    {
        bufferSwitchFlag = true;
    }

    pBuffer[bufferPos++] = value;
    bufferPos &= bufferMask;
    tapInfo.position++;
    tapInfo.length = tapInfo.position;
}

void TapRecorder::CalcTapData(uint32_t signalTime)
{
    /*
    if (tapInfo.version == 2)
    {
        // for TAP format 2 (half-wave) each half of the signal is represented indivudually and so is returned as double the length of a version 0 or 1 value
        // so that the 2 Mhz counter doesn't have to do the work. The alternative would be to halve the version 0 or 1 values.
        signalTime >>= 1;
    }
    */
    // TODO: check this for accuracy, signal time is uS, cycle time is machine dependent.
    //       however counter calculation is definitely uS based, both rec and play use uS so perhaps cycles is not the correct
    //       name for this.
    tapInfo.cycles += signalTime;

    uint32_t tapData = (uint32_t) ((double) signalTime / cycleMult8);
    if (tapData < 256)
    {
        WriteByte((uint8_t) tapData);
    }
    else
    {
        // in version 0 TAP files a zero is used to indicate an overflow condition
        WriteByte(0);
        // otherwise the zero is followed by 24 bits of signal data
        if (tapInfo.version != 0)
        {
            uint32_t tapData = (uint32_t) ((double) signalTime / cycleMultRaw);
            WriteByte((uint8_t) tapData);
            WriteByte((uint8_t) (tapData >> 8));
            WriteByte((uint8_t) (tapData >> 16));
        }
    }
}

inline void TapRecorder::FlushBufferIfNeeded(File tapFile)
{
    if (bufferSwitchFlag)
    {
        bufferSwitchFlag = false;
        bufferSwitchPos = halfBufferSize - bufferSwitchPos;
        tapFile.write(&pBuffer[bufferSwitchPos], halfBufferSize);
    }
}

void TapRecorder::FlushBufferFinal(File tapFile)
{
    if (bufferPos < halfBufferSize)
    {
        tapFile.write(pBuffer, bufferPos);
    }
    else
    {
        tapFile.write(&pBuffer[halfBufferSize], bufferPos - halfBufferSize);
    }
    tapFile.seek(TAP_HEADER_MAGIC_LENGTH);
    tapFile.write((uint8_t*) &tapInfo, TAP_HEADER_DATA_LENGTH);
}

void TapRecorder::StartSampling()
{
    processSignal = true;
    HWStartSampling();
    // tell the C64 that play has been pressed
    digitalWrite(C64_SENSE_PIN, LOW);
}

void TapRecorder::StopSampling()
{
    // prevent any further buffer processing
    processSignal = false;
    // shutdown the hardware timer
    HWStopSampling();

    // tell the C64 that stop has been pressed
    digitalWrite(C64_SENSE_PIN, HIGH);
}

ErrorCodes TapRecorder::CreateTap(File tapFile)
{
    uint32_t tap_magic[TAP_HEADER_MAGIC_LENGTH / 4];
    memset(&tapInfo, 0, sizeof(tapInfo));

    if (pBuffer == NULL)
    {
        pBuffer = (uint8_t*) malloc(bufferSize);
        if (pBuffer == NULL)
            return ErrorCodes::OUT_OF_MEMORY;
    }

    bufferPos = 0;
    tapFile.seek(0);

    // TODO: C16 recording is not properly implemented as yet.
    // The user can set the value to the C16 machine type, but half-wave recording WILL NOT OCCUR!!!
    MACHINE_TYPE machineType = (MACHINE_TYPE) options->machineType.GetValue();

    tap_magic[0] = machineType == MACHINE_TYPE::C16 ? TAP_MAGIC_C16 : TAP_MAGIC_C64;
    // check the post fix for "TAPE-RAW", use a 4-byte magic trick
    tap_magic[1] = TAP_MAGIC_POSTFIX1;
    tap_magic[2] = TAP_MAGIC_POSTFIX2;

    tapInfo.version = machineType == MACHINE_TYPE::C16 ? 2 : 1;
    tapInfo.video = (uint8_t) (options->ntscPAL.GetValue() ? VIDEO_MODE::NSTC : VIDEO_MODE::PAL);
    tapInfo.platform = (uint8_t) machineType;

    if (tapFile.write((uint8_t*) &tap_magic, TAP_HEADER_MAGIC_LENGTH) != TAP_HEADER_MAGIC_LENGTH)
        return ErrorCodes::FILE_ERROR;

    if (tapFile.write((uint8_t*) &tapInfo, TAP_HEADER_DATA_LENGTH) != TAP_HEADER_DATA_LENGTH)
        return ErrorCodes::FILE_ERROR;

    SetupCycleTiming();

    return (ErrorCodes::OK);
}

bool TapRecorder::InRecordMenu(File tapFile)
{
    MenuHandler menu(lcdUtils, inputHandler);

    MenuEntry inRecordEntries[] = {
        {MenuEntryType::IndexEntry, S_CONTINUE, NULL},
        {MenuEntryType::IndexEntry, S_EXIT, NULL},
    };
    TheMenu inPlayMenu = {S_RECORD_MENU, (MenuEntry*) inRecordEntries, 2, 0, NULL};

    switch (menu.Display(&inPlayMenu))
    {
        // Play
        case 0:
        {
            return (true);
        }
        // Exit (1) or Abort (-1)
        default:
            return (false);
    }
}

void TapRecorder::RecordTap(File tapFile)
{
    ErrorCodes ret = CreateTap(tapFile);
    if (ret != ErrorCodes::OK)
    {
        lcdUtils->Error(S_CREATE_FAILED, "");
        Serial.printf("Unable to create: %s, %d\n", tapFile.name(), (int) ret);
        return;
    };

    lcdUtils->Title(S_SELECT_TO_REC);
    lcdUtils->ShowFile(tapFile.name(), false);

    while (inputHandler->GetInput() != InputResponse::Select)
    {
        delay(10);
    }

    delay(1000);
    lcdUtils->Title(S_RECORDING);

    StartSampling();

    while (true)
    {
        motorOn = digitalRead(C64_MOTOR_PIN);
        if (!processSignal)
            break;

        FlushBufferIfNeeded(tapFile);

        // (uint16_t) (DS_G * (sqrt((tapInfo.cycles / 1000000.0 * (DS_V_PLAY / DS_D / PI)) + ((DS_R * DS_R) / (DS_D * DS_D))) - (DS_R / DS_D)));
        tapInfo.counterActual = CYCLES_TO_COUNTER(tapInfo.cycles);
        lcdUtils->PlayUI(motorOn, tapInfo.counterActual, 200);

        InputResponse resp = inputHandler->GetInput();

        if (resp == InputResponse::Select || resp == InputResponse::Abort)
        {
            StopSampling();
            if (resp == InputResponse::Abort)
            {
                FlushBufferFinal(tapFile);
                return;
            }
            if (!InRecordMenu(tapFile))
            {
                Serial.println("exiting recording");
                FlushBufferFinal(tapFile);
                return;
            }
            lcdUtils->Title(S_RECORDING);
            lcdUtils->ShowFile(tapFile.name(), false);
            StartSampling();
        }
    }
}
