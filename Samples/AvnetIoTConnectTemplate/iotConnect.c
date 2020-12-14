// This file implements the logic required to connect and interface with Avnet's IoTConnect platform

#include "iotConnect.h"

// IoT Connect defines.
#ifdef PARSE_ALL_IOTC_PARMETERS
static uint8_t ecValue;
static uint8_t ctValue;
static uint8_t hasDValue;
static uint8_t hasAttrValue;
static uint8_t hasSetValue;
static uint8_t hasRValue;
#endif

// Declare global variables
static char dtgGUID[GUID_LEN + 1];
static char gGUID[GUID_LEN + 1];
static char sidString[SID_LEN + 1];
static bool IoTCConnected = false;

static EventLoopTimer *IoTCTimer = NULL;
static int IoTCHelloTimer = -1;
static const int IoTCDefaultPollPeriodSeconds =
    15; // Wait for 15 seconds for IoT Connect to send first response

// Forwared function declarations
static void IoTCTimerEventHandler(EventLoopTimer *timer);
static void IoTCsendIoTCHelloTelemetry(void);
static IOTHUBMESSAGE_DISPOSITION_RESULT receiveMessageCallback(IOTHUB_MESSAGE_HANDLE, void *);
static bool ReadSIDFromMutableFile(char *);

// Call when first connected to the IoT Hub
void IoTConnectConnectedToIoTHub(void)
{

    IoTHubDeviceClient_LL_SetMessageCallback(iothubClientHandle, receiveMessageCallback, NULL);

    // Since we're going to be connecting or re-connecting to Azure
    // Set the IoT Connected flag to false
    IoTCConnected = false;

    // Send the IoT Connect hello message to inform the platform that we're on-line!
    IoTCsendIoTCHelloTelemetry();

    // Start the timer to make sure we see the IoT Connect "first response"
    const struct timespec IoTCHelloPeriod = {.tv_sec = IoTCDefaultPollPeriodSeconds, .tv_nsec = 0};
    SetEventLoopTimerPeriod(IoTCTimer, &IoTCHelloPeriod);
}

// Call from the main init function to setup periodic handler
ExitCode IoTConnectInit(void)
{

    IoTCHelloTimer = IoTCDefaultPollPeriodSeconds;
    struct timespec IoTCHelloPeriod = {.tv_sec = IoTCHelloTimer, .tv_nsec = 0};
    IoTCTimer = CreateEventLoopPeriodicTimer(eventLoop, &IoTCTimerEventHandler, &IoTCHelloPeriod);
    if (IoTCTimer == NULL) {
        return ExitCode_Init_IoTCTimer;
    }

    // Read the sid from flash memory.  If we have not written an sid to
    // memory yet, the sidString variable will be empty and we can still
    // send it to IoTConnect.
    ReadSIDFromMutableFile(sidString);
    return ExitCode_Success;
}

/// <summary>
/// IoTConnect timer event:  Check for response status and send hello message
/// </summary>
static void IoTCTimerEventHandler(EventLoopTimer *timer)
{
    if (!IoTCConnected) {
        Log_Debug("Check to see if we need to send the IoTC Hello message\n");

        if (ConsumeEventLoopTimerEvent(timer) != 0) {
            exitCode = ExitCode_IoTCTimer_Consume;
            return;
        }

        bool isNetworkReady = false;
        if (Networking_IsNetworkingReady(&isNetworkReady) != -1) {
            if (IsConnectionReadyToSendTelemetry()) {
                if (!IoTCConnected) {
                    IoTCsendIoTCHelloTelemetry();
                }
            }
        } else {
            Log_Debug("Failed to get Network state\n");
        }
    }
}

/// <summary>
/// Write an character sid string to this application's persistent data file
/// </summary>
static void WriteSIDToMutableFile(char *sid)
{

    int fd = Storage_OpenMutableFile();
    if (fd == -1) {
        Log_Debug("ERROR: Could not open mutable file:  %s (%d).\n", strerror(errno), errno);
        exitCode = ExitCode_WriteFile_OpenMutableFile;
        return;
    }
    ssize_t ret = write(fd, sid, SID_LEN);
    if (ret == -1) {
        // If the file has reached the maximum size specified in the application manifest,
        // then -1 will be returned with errno EDQUOT (122)
        Log_Debug("ERROR: An error occurred while writing to mutable file:  %s (%d).\n",
                  strerror(errno), errno);
        exitCode = ExitCode_WriteFile_Write;
    } else if (ret < SID_LEN) {
        // For simplicity, this sample logs an error here. In the general case, this should be
        // handled by retrying the write with the remaining data until all the data has been
        // written.
        Log_Debug("ERROR: Only wrote %d of %d bytes requested\n", ret, SID_LEN);
    }
    close(fd);
}

/// <summary>
/// Read a sid string from this application's persistent data file
/// </summary>
/// <returns>
/// The sid string that was read from the file.  If the file is empty, this returns 0.  If the
/// storage API fails, this returns -1.
/// </returns>
static bool ReadSIDFromMutableFile(char *sid)
{
    int fd = Storage_OpenMutableFile();
    if (fd == -1) {
        Log_Debug("ERROR: Could not open mutable file:  %s (%d).\n", strerror(errno), errno);
        exitCode = ExitCode_ReadFile_OpenMutableFile;
        return false;
    }

    ssize_t ret = read(fd, sid, SID_LEN);
    if (ret == -1) {
        Log_Debug("ERROR: An error occurred while reading file:  %s (%d).\n", strerror(errno),
                  errno);
        exitCode = ExitCode_ReadFile_Read;
    }
    close(fd);

    if (ret < SID_LEN) {
        return false;
    }

    return true;
}

/// <summary>
///     Callback function invoked when a message is received from IoT Hub.
/// </summary>
/// <param name="message">The handle of the received message</param>
/// <param name="context">The user context specified at IoTHubDeviceClient_LL_SetMessageCallback()
/// invocation time</param>
/// <returns>Return value to indicates the message procession status (i.e. accepted, rejected,
/// abandoned)</returns>
static IOTHUBMESSAGE_DISPOSITION_RESULT receiveMessageCallback(IOTHUB_MESSAGE_HANDLE message,
                                                               void *context)
{
#ifdef ENABLE_IOTC_MESSAGE_DEBUG
    Log_Debug("Received message!\n");
#endif

    const unsigned char *buffer = NULL;
    size_t size = 0;
    if (IoTHubMessage_GetByteArray(message, &buffer, &size) != IOTHUB_MESSAGE_OK) {
        Log_Debug("WARNING: failure performing IoTHubMessage_GetByteArray\n");
        return IOTHUBMESSAGE_REJECTED;
    }

    // 'buffer' is not zero terminated, so null terminate it.
    unsigned char *str_msg = (unsigned char *)malloc(size + 1);
    if (str_msg == NULL) {
        Log_Debug("ERROR: could not allocate buffer for incoming message\n");
        abort();
    }
    memcpy(str_msg, buffer, size);
    str_msg[size] = '\0';

#ifdef ENABLE_IOTC_MESSAGE_DEBUG
    Log_Debug("INFO: Received message '%s' from IoT Hub\n", str_msg);
#endif

    // Process the message.  We're expecting a specific JSON structure from IoT Connect
    //
    //{
    //    "d": {
    //        "ec": 0,
    //            "ct" : 200,
    //            "dtg" : "b3a7d542-20ad-4397-abf3-5d7ec539fba6",  // A GUID
    //            "sid" : "9tAyZNOIWD+1D2Qp785FDsXUmrEnGJntnAvV1uSxKSSRL4ZaLgo5UV1hRY0kTmHg", // 64
    //            character string "g" : "c2fbe330-8787-4dbd-87e4-9ecf58c41f6a", // A GUID has":{
    //            "d" : 1,
    //          "attr" : 1,
    //          "set" : 1,
    //          "r" : 1
    //         }
    //      }
    //  }
    //
    // The code below will drill into the structure and pull out each piece of data and store it
    // into variables

    // Using the mesage string get a pointer to the rootMessage
    JSON_Value *rootMessage = NULL;
    rootMessage = json_parse_string(str_msg);
    if (rootMessage == NULL) {
        Log_Debug("WARNING: Cannot parse the string as JSON content.\n");
        goto cleanup;
    }

    // Using the rootMessage pointer get a pointer to the rootObject
    JSON_Object *rootObject = json_value_get_object(rootMessage);

    // Using the root object get a pointer to the d object
    JSON_Object *dProperties = json_object_dotget_object(rootObject, "d");
    if (dProperties == NULL) {
        Log_Debug("dProperties == NULL\n");
    }

#ifdef PARSE_ALL_IOTC_PARMETERS
    // The d properties should have a "ec" key
    if (json_object_has_value(dProperties, "ec") != 0) {
        ecValue = (uint8_t)json_object_get_number(dProperties, "ec");
        Log_Debug("ec: %d\n", ecValue);
    } else {
        Log_Debug("ec not found!\n");
    }

    // The d properties should have a "ct" key
    if (json_object_has_value(dProperties, "ct") != 0) {
        ctValue = (uint8_t)json_object_get_number(dProperties, "ct");
        Log_Debug("ct: %d\n", ctValue);
    } else {
        Log_Debug("ct not found!\n");
    }
#endif

    // The d properties should have a "dtg" key
    if (json_object_has_value(dProperties, "dtg") != 0) {
        strncpy(dtgGUID, (char *)json_object_get_string(dProperties, "dtg"), GUID_LEN);
#ifdef ENABLE_IOTC_MESSAGE_DEBUG
        Log_Debug("dtg: %s\n", dtgGUID);
#endif
    } else {
        Log_Debug("dtg not found!\n");
    }

    // The d properties should have a "sid" key
    if (json_object_has_value(dProperties, "sid") != 0) {
        char newSIDString[64 + 1];
        strncpy(newSIDString, (char *)json_object_get_string(dProperties, "sid"), SID_LEN);
#ifdef ENABLE_IOTC_MESSAGE_DEBUG
        Log_Debug("sid: %s\n", newSIDString);
#endif

        if (strncmp(newSIDString, sidString, SID_LEN) != 0) {
#ifdef ENABLE_IOTC_MESSAGE_DEBUG
            Log_Debug("sid string is different, write the new string to Flash\n");
#endif
            WriteSIDToMutableFile(newSIDString);
            strncpy(sidString, newSIDString, SID_LEN);
        }
#ifdef ENABLE_IOTC_MESSAGE_DEBUG
        else {
            Log_Debug("sid string did not change!\n");
        }
#endif
    } else {
#ifdef ENABLE_IOTC_MESSAGE_DEBUG
        Log_Debug("sid not found!\n");
#endif
    }

    // The d properties should have a "g" key
    if (json_object_has_value(dProperties, "g") != 0) {
        strncpy(gGUID, (char *)json_object_get_string(dProperties, "g"), GUID_LEN);
#ifdef ENABLE_IOTC_MESSAGE_DEBUG
        Log_Debug("g: %s\n", gGUID);
#endif
    } else {
        Log_Debug("dtg not found!\n");
    }

#ifdef PARSE_ALL_IOTC_PARMETERS

    // The d object has a "has" object
    JSON_Object *hasProperties = json_object_dotget_object(dProperties, "has");
    if (hasProperties == NULL) {
        Log_Debug("hasProperties == NULL\n");
    }

    // The "has" properties should have a "d" key
    if (json_object_has_value(hasProperties, "d") != 0) {
        hasDValue = (uint8_t)json_object_get_number(hasProperties, "d");
        Log_Debug("has:d: %d\n", hasDValue);
    } else {
        Log_Debug("has:d not found!\n");
    }

    // The "has" properties should have a "attr" key
    if (json_object_has_value(hasProperties, "attr") != 0) {
        hasAttrValue = (uint8_t)json_object_get_number(hasProperties, "attr");
        Log_Debug("has:attr: %d\n", hasAttrValue);
    } else {
        Log_Debug("has:attr not found!\n");
    }

    // The "has" properties should have a "set" key
    if (json_object_has_value(hasProperties, "set") != 0) {
        hasSetValue = (uint8_t)json_object_get_number(hasProperties, "set");
        Log_Debug("has:set: %d\n", hasSetValue);
    } else {
        Log_Debug("has:set not found!\n");
    }

    // The "has" properties should have a "r" key
    if (json_object_has_value(hasProperties, "r") != 0) {
        hasRValue = (uint8_t)json_object_get_number(hasProperties, "r");
        Log_Debug("has:r %d\n", hasRValue);
    } else {
        Log_Debug("has:r not found!\n");
    }
#endif

    // Since we just processed the IoTConnect message, set the IoTConnected flag to true
    IoTCConnected = true;

cleanup:
    // Release the allocated memory.
    json_value_free(rootMessage);
    free(str_msg);

    return IOTHUBMESSAGE_ACCEPTED;
}

/// <summary>
///     Function to generate the date and time string required by IoT Connect
/// </summary>
/// <param name="stringStorage">A character array that can hold 29 characters</param>
/// <returns>Fills the passed in character array with the current date and time</returns>
void getTimeString(char *stringStorage)
{
    // Generate the required "dt" time string in the correct format
    time_t now;
    time(&now);

    strftime(stringStorage, sizeof("2020-06-23T15:27:33Z"), "%FT%TZ", gmtime(&now));

    // strftime has provided the year, month, day, hour, minute and second details.
    // Fill in the remaining required time string with ".00000000Z"  We overwite
    // the 'Z' at the end of the original string but replace it at the end of the
    // modified string.  Null terminate the string.
    char timeFiller[] = {".0000000Z\0"};
    size_t timeFillerLen = sizeof(timeFiller);
    strncpy(&stringStorage[19], timeFiller, timeFillerLen);
    return;
}

void IoTCsendIoTCHelloTelemetry(void)
{

    // Send the IoT Connect hello message to inform the platform that we're on-line!

    // Declare a buffer for the time string and call the routine to put the current date time string
    // into the buffer
    char timeBuffer[29];
    getTimeString(timeBuffer);

    // Declare a buffer for the hello message and construct the message
    char telemetryBuffer[IOTC_HELLO_TELEMETRY_SIZE];
    int len = snprintf(telemetryBuffer, IOTC_HELLO_TELEMETRY_SIZE,
                       "{\"t\": \"%s\",\"mt\" : 200,\"sid\" : \"%s\"}", timeBuffer, sidString);
    if (len < 0 || len >= IOTC_HELLO_TELEMETRY_SIZE) {
        Log_Debug("ERROR: Cannot write telemetry to buffer.\n");
        return;
    }
    SendTelemetry(telemetryBuffer);
}

// Construct a new message that contains all the required IoTConnect data and the original telemetry
// message Returns false if we have not received the first response from IoTConnect, or if the
// target buffer is not large enough
bool FormatTelemetryForIoTConnect(const char *originalJsonMessage, char *modifiedJsonMessage,
                                  size_t modifiedBufferSize)
{

    // Define the Json string format for sending telemetry to IoT Connect, note that the
    // actual telemetry data is inserted as the last string argument
    static const char IoTCTelemetryJson[] =
        "{\"sid\":\"%s\",\"dtg\":\"%s\",\"mt\": 0,\"dt\": \"%s\",\"d\":[{\"d\":%s}]}";

    // Verify that we've received the initial handshake response from IoTConnect, if not return
    // false
    if (!IoTCConnected) {
        Log_Debug(
            "Can't construct IoTConnect Telemetry message because application has not received the "
            "initial IoTConnect handshake\n");
        return false;
    }

    // Determine the largest message size needed.  We'll use this to validate the incomming target
    // buffer is large enough
    size_t maxModifiedMessageSize = strlen(originalJsonMessage) + IOTC_TELEMETRY_OVERHEAD;

    // Verify that the passed in buffer is large enough for the modified message
    if (maxModifiedMessageSize > modifiedBufferSize) {
        Log_Debug(
            "\nERROR: FormatTelemetryForIoTConnect() modified buffer size can't hold modified "
            "message\n");
        Log_Debug("                 Original message size: %d\n", strlen(originalJsonMessage));
        Log_Debug("Additional IoTConnect message overhead: %d\n", IOTC_TELEMETRY_OVERHEAD);
        Log_Debug("           Required target buffer size: %d\n", maxModifiedMessageSize);
        Log_Debug("             Actural target buffersize: %d\n\n", modifiedBufferSize);
        return false;
    }

    // Build up the IoTC message and insert the telemetry JSON

    // Generate the required "dt" time string in the correct format
    time_t now;
    time(&now);
    char timeBuffer[sizeof "2020-06-23T15:27:33.0000000Z "];
    strftime(timeBuffer, sizeof(timeBuffer), "%FT%TZ", gmtime(&now));

    // strftime has provided the year, month, day, hour, minute and second details.
    // Fill in the remaining required time string with ".00000000Z"  We overwite
    // the 'Z' at the end of the original string but replace it at the end of the
    // modified string.  Null terminate the string.
    char timeFiller[] = {".0000000Z\0"};
    size_t timeFillerLen = sizeof(timeFiller);
    strncpy(&timeBuffer[19], timeFiller, timeFillerLen);

    // construct the telemetry message
    snprintf(modifiedJsonMessage, maxModifiedMessageSize, IoTCTelemetryJson, sidString, dtgGUID,
             timeBuffer, originalJsonMessage);

    //    Log_Debug("Original message: %s\n", originalJsonMessage);
    //    Log_Debug("Returning message: %s\n", modifiedJsonMessage);

    return true;
}