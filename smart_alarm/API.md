# API Documentation - ESP32 Smart Timer

## REST API Reference

All requests are made to: `http://192.168.4.1` (ESP32 AP mode)

---

## Endpoints Overview

| Method | Endpoint | Purpose | Returns |
|--------|----------|---------|---------|
| GET | `/` | Get web UI | HTML page |
| GET | `/api/time` | Get current RTC time | JSON |
| GET | `/api/settime` | Set RTC time | JSON |
| GET | `/api/periods` | List scheduled periods + timer status | JSON (array) |
| GET | `/api/addperiod` | Add a new scheduled period | JSON |
| GET | `/api/deleteperiod` | Delete a scheduled period by index | JSON |
| GET | `/api/editperiod` | Modify an existing period | JSON |
| GET | `/api/starttimer` | Activate the scheduler | JSON |
| GET | `/api/stoptimer` | Deactivate the scheduler | JSON |
| GET | `/api/trigger` | Manually trigger output for delay seconds | JSON |

---

## 1. GET `/`

### Description
Returns the HTML/CSS/JavaScript web interface for browser viewing.

### Request
```
GET http://192.168.4.1
```

### Response
- **Status**: 200 OK
- **Content-Type**: text/html
- **Body**: Complete HTML page (~ 20KB)

### Example (Browser)
```
Open: http://192.168.4.1
Shows: Interactive timer control UI
```

---

## 2. GET `/api/time`

### Description
Returns the current time from the DS3231 RTC module.

### Request
```
GET http://192.168.4.1/api/time
```

### Response
```json
{
  "hour": 14,
  "minute": 35,
  "second": 42
}
```

### Parameters
- None

### Response Fields
| Field | Type | Range | Description |
|-------|------|-------|-------------|
| hour | int | 0-23 | Current hour (24-hour format) |
| minute | int | 0-59 | Current minute |
| second | int | 0-59 | Current second |

### Example Curl
```bash
curl http://192.168.4.1/api/time
```

### JavaScript Fetch
```javascript
fetch('http://192.168.4.1/api/time')
  .then(response => response.json())
  .then(data => console.log(`${data.hour}:${data.minute}:${data.second}`));
```

---

## 3. GET `/api/settime`

### Description
Sets the current time on the DS3231 RTC module.

### Request
```
GET http://192.168.4.1/api/settime?hour=14&minute=35&second=30
```

### Query Parameters
| Parameter | Type | Range | Required | Description |
|-----------|------|-------|----------|-------------|
| hour | int | 0-23 | Yes | Hour to set |
| minute | int | 0-59 | Yes | Minute to set |
| second | int | 0-59 | Yes | Second to set |

### Response (Success)
```json
{
  "success": true
}
```

### Response (Error)
```json
{
  "success": false
}
```

### Example Curl
```bash
curl "http://192.168.4.1/api/settime?hour=14&minute=35&second=30"
```

### JavaScript Fetch
```javascript
const hour = 14, minute = 35, second = 30;
fetch(`http://192.168.4.1/api/settime?hour=${hour}&minute=${minute}&second=${second}`)
  .then(response => response.json())
  .then(data => {
    if (data.success) {
      console.log("Time set successfully!");
    }
  });
```


## 4. GET `/api/periods`

### Description
Return the current list of scheduled periods (sorted by day/time) and the timer active flag.
Each period object includes its original index in the stored list which should be used when
calling edit/delete operations.

### Request
```
GET http://192.168.4.1/api/periods
```

### Response
```json
{
  "periods": [
    {"index":0,"day":1,"hour":8,"minute":0,"second":0,"delaySeconds":60},
    {"index":2,"day":2,"hour":12,"minute":30,"second":0,"delaySeconds":300}
  ],
  "timerActive": true
}
```

### Notes
- Day numbering: 0=Sunday, 1=Monday, etc.
- The server sorts the array; the index field points back to the original entry.

---

## 5. GET `/api/addperiod`

### Description
Create a new scheduled period. The timer does not need to be active to add or modify periods.

### Request
```
GET http://192.168.4.1/api/addperiod?day=1&hour=8&minute=0&second=0&delaySeconds=60
```

### Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| day | int | Yes | Day of week (0=Sun..6=Sat) |
| hour | int | Yes | Hour (0-23) |
| minute | int | Yes | Minute (0-59) |
| second | int | Yes | Second (0-59) |
| delaySeconds | int | Yes | Duration in seconds |

### Response
```json
{ "success": true }
```

---

## 6. GET `/api/deleteperiod`

### Description
Remove an existing period using the original index value returned by `/api/periods`.

### Request
```
GET http://192.168.4.1/api/deleteperiod?index=0
```

### Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| index | int | Yes | Original index of period to delete |

### Response
```json
{ "success": true }
```

---

## 7. GET `/api/editperiod`

### Description
Modify an existing period. Provide the index and the new values.

### Request
```
GET http://192.168.4.1/api/editperiod?index=0&day=2&hour=9&minute=15&second=0&delaySeconds=120
```

### Parameters
Same as `/api/addperiod` plus `index`.

### Response
```json
{ "success": true }
```

---

## 8. GET `/api/starttimer`

### Description
Enable the scheduler; upcoming periods will trigger the output when they arrive.

### Request
```
GET http://192.168.4.1/api/starttimer
```

### Response
```json
{ "success": true }
```

---

## 9. GET `/api/stoptimer`

### Description
Disable the scheduler and immediately turn the output off.

### Request
```
GET http://192.168.4.1/api/stoptimer
```

### Response
```json
{ "success": true }
```

---

## 10. GET `/api/trigger`

### Description
Manually activate the output for a specified number of seconds (useful for real‑time testing).

### Request
```
GET http://192.168.4.1/api/trigger?delaySeconds=30
```

### Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| delaySeconds | int | Yes | Duration (0–60 seconds) |

### Response
```json
{ "success": true }
```};

const queryString = new URLSearchParams(params).toString();
fetch(`http://192.168.4.1/api/settimer?${queryString}`)
  .then(response => response.json())
  .then(data => {
    if (data.success) {
      console.log("Timer configured!");
    }
  });
```

### Trigger Logic
```
Trigger Time = Base Time + Delay
Example: 14:35:00 + 00:05:00 = 14:40:00 (triggers at this time)
```

### Notes
- All values are required
- Returns `false` if any parameter is invalid
- Timer resets status to "idle" (awaiting trigger)
- Settings persist in EEPROM

---

## 6. GET `/api/starttimer`

### Description
Activates the timer scheduling system.

### Request
```
GET http://192.168.4.1/api/starttimer
```

### Response (Success)
```json
{
  "success": true
}
```

### Response (Error)
```json
{
  "success": false
}
```

### Parameters
- None

### Example Curl
```bash
curl http://192.168.4.1/api/starttimer
```

### JavaScript Fetch
```javascript
fetch('http://192.168.4.1/api/starttimer')
  .then(response => response.json())
  .then(data => {
    if (data.success) {
      console.log("Timer started! Waiting for trigger time...");
    }
  });
```

### Behavior When Started
1. Timer status changes to ACTIVE
2. ESP32 continuously checks current time vs trigger time
3. When trigger time is reached:
   - GPIO 26 goes HIGH
   - Output stays high for 5 seconds
   - Then returns to LOW
   - Status changes to TRIGGERED
4. Display shows flashing effect during trigger

### Notes
- Must have timer configured first (via `/api/settimer`)
- Status persists in EEPROM
- Can be called multiple times (idempotent)

---

## 7. GET `/api/stoptimer`

### Description
Deactivates the timer scheduling system.

### Request
```
GET http://192.168.4.1/api/stoptimer
```

### Response (Success)
```json
{
  "success": true
}
```

### Response (Error)
```json
{
  "success": false
}
```

### Parameters
- None

### Example Curl
```bash
curl http://192.168.4.1/api/stoptimer
```

### JavaScript Fetch
```javascript
fetch('http://192.168.4.1/api/stoptimer')
  .then(response => response.json())
  .then(data => {
    if (data.success) {
      console.log("Timer stopped!");
    }
  });
```

### Behavior When Stopped
1. Timer status changes to INACTIVE
2. GPIO 26 is immediately set to LOW
3. Any active relay/buzzer output stops
4. Display shows "INACTIVE" status
5. Timer stops checking trigger conditions

### Notes
- Can be called at any time
- Immediately stops all outputs
- Status persists in EEPROM
- Can be called multiple times (idempotent)

---

## Common Use Cases

### Use Case 1: Simple Bell/Alarm

```javascript
// Set time to 9:00 AM, trigger immediately
fetch('http://192.168.4.1/api/settimer?setHour=9&setMinute=0&setSecond=0&delayHour=0&delayMinute=0&delaySecond=0');
fetch('http://192.168.4.1/api/starttimer');
```

### Use Case 2: Delayed Activation

```javascript
// Current time: 2:30 PM, trigger in 15 minutes (2:45 PM)
fetch('http://192.168.4.1/api/settimer?setHour=14&setMinute=30&setSecond=0&delayHour=0&delayMinute=15&delaySecond=0');
fetch('http://192.168.4.1/api/starttimer');
```

### Use Case 3: Multiple Schedules

```javascript
// First schedule
fetch('http://192.168.4.1/api/settimer?setHour=9&setMinute=0&setSecond=0&delayHour=0&delayMinute=0&delaySecond=0');
fetch('http://192.168.4.1/api/starttimer');

// ... wait until triggered ...
// Then manually change for next schedule
fetch('http://192.168.4.1/api/settimer?setHour=10&setMinute=0&setSecond=0&delayHour=0&delayMinute=0&delaySecond=0');
fetch('http://192.168.4.1/api/starttimer');
```

---

## Error Handling

### Possible Errors:

| Scenario | Response | Resolution |
|----------|----------|-----------|
| Invalid parameter | `{"success": false}` | Check ranges (hour 0-23, min/sec 0-59) |
| Missing parameter | `{"success": false}` | Include all required query parameters |
| RTC not found | Error in Serial Monitor | Check I2C connections |
| Device unreachable | Connection refused | Verify WiFi connection to SHIBU_TIMER |
| Invalid endpoint | 404 Not Found | Check endpoint spelling in request |

### HTTP Status Codes

- **200 OK**: Request successful
- **400 Bad Request**: Invalid parameters
- **404 Not Found**: Endpoint doesn't exist

---

## Response Headers

All responses include these headers:

```
HTTP/1.1 200 OK
Content-Type: application/json
Content-Length: [size]
Connection: keep-alive
```

---

## Rate Limiting

- **No rate limiting** in current implementation
- Safe to call `/api/time` every second
- Safe to call `/api/timer` every 2 seconds
- Avoid calling `/api/settimer` more than once per minute

---

## Testing with cURL

### Test 1: Get Current Time
```bash
curl http://192.168.4.1/api/time
# Response: {"hour":14,"minute":35,"second":42}
```

### Test 2: Set Time
```bash
curl "http://192.168.4.1/api/settime?hour=14&minute=35&second=30"
# Response: {"success":true}
```

### Test 3: Configure and Start Timer
```bash
curl "http://192.168.4.1/api/settimer?setHour=14&setMinute=40&setSecond=0&delayHour=0&delayMinute=0&delaySecond=0"
curl http://192.168.4.1/api/starttimer
```

### Test 4: Stop Timer
```bash
curl http://192.168.4.1/api/stoptimer
```

---

## Testing with Python

```python
import requests

# Get current time
response = requests.get('http://192.168.4.1/api/time')
print(response.json())

# Set time to 14:35:30
requests.get('http://192.168.4.1/api/settime?hour=14&minute=35&second=30')

# Configure timer
requests.get('http://192.168.4.1/api/settimer?setHour=14&setMinute=40&setSecond=0&delayHour=0&delayMinute=0&delaySecond=0')

# Start timer
response = requests.get('http://192.168.4.1/api/starttimer')
print(response.json())

# Stop timer
requests.get('http://192.168.4.1/api/stoptimer')
```

---

## Testing with JavaScript (Node.js)

```javascript
const http = require('http');

// Helper function to make requests
function makeRequest(endpoint) {
  return new Promise((resolve, reject) => {
    http.get(`http://192.168.4.1${endpoint}`, (res) => {
      let data = '';
      res.on('data', chunk => data += chunk);
      res.on('end', () => {
        try {
          resolve(JSON.parse(data));
        } catch (e) {
          resolve(data);
        }
      });
    });
  });
}

// Usage
(async () => {
  const time = await makeRequest('/api/time');
  console.log('Current time:', time);
  
  await makeRequest('/api/settime?hour=14&minute=35&second=30');
  await makeRequest('/api/settimer?setHour=14&setMinute=40&setSecond=0&delayHour=0&delayMinute=0&delaySecond=0');
  await makeRequest('/api/starttimer');
})();
```

---

## Performance

- **Response time**: < 50ms for most endpoints
- **Maximum concurrent requests**: 1 (single thread web server)
- **Data persistence**: EEPROM write takes ~10ms

---

**Last Updated**: February 2026  
**API Version**: 1.0
