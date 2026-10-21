#pragma once
struct MockVector { float x = 0, y = 0, z = 0; };
struct sensors_event_t { MockVector acceleration, gyro; };
