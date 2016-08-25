#include <ros/ros.h>
#include <ros/service.h>
#include <serial/serial.h>
#include <stdlib.h>
#include <string>
#include <angles/angles.h>

#include <rs_ros_sensor_board/ultrasnd_bump_ranges.h>
#include <rs_ros_sensor_board/camera_light.h>

using namespace std;
using namespace rs_ros_sensor_board;

#define MAX_USND_SENS 4

#define INVALID_REMAP 4.0f

#define	MSG_ULTRASOUND	0x01
#define MSG_PWM			0X02

#define CTRL_WORD_0	0xA55A
#define CTRL_WORD_1 0x0D0A

typedef struct _ultrasnd_data_out
{
    uint16_t ctrl_frame_0;		// 0xA55A
    uint8_t byte_count;   		// number of bytes following
    uint8_t type;				// Message type
    uint32_t ticks;       		// ticks since system start
    float not_valid_val;  		// value for not valid distances
    float distances[MAX_USND_SENS];	// distances in meters
    uint16_t sonar_active; 		// Number of sonar connected;
    uint16_t ctrl_frame_1;  	// 0x0D0A
} UltraSndDataOut; // 20 bytes

typedef struct _pwm_data
{
    uint16_t ctrl_frame_0;	// 0xA55A
    uint8_t byte_count;   	// number of bytes following
    uint8_t type;			// Message type
    float frequency;  		// Frequency of the PWM
    float dutyCycle;   		// Duty Cycle
    uint16_t ctrl_frame_1;  // 0x0D0A
} PwmData; // 14 bytes


// >>>>> Global functions
void loadParams();
bool connect_serial();
// <<<<< Global functions

// >>>>> Global variables
ros::NodeHandle* nh;
ros::NodeHandle* nhPriv;

string serial_port_name;
int baudrate;
int timeout_msec;

bool simul;

serial::Serial serPort;
// <<<<< Global variables

#define DEFAULT_SER_PORT    "/dev/ttyUSB0"
#define DEFAULT_BAUDRATE    115200
#define DEFAULT_TIMEOUT     500

bool changeLight( camera_light::Request  &req,
                  camera_light::Response &res)
{
    if( req.lightPwmFreq > 32767.0f ||
            req.lightPwmDutyCycle > 1.0f )
    {
        ROS_WARN_STREAM( "Wrong Light PWM parameter" );
        res.settingOk = false;
        return false;
    }

    PwmData lightPwmMsg;

    lightPwmMsg.ctrl_frame_0 = CTRL_WORD_0;
    lightPwmMsg.byte_count = 11;
    lightPwmMsg.type = MSG_PWM;
    lightPwmMsg.frequency = req.lightPwmFreq;
    lightPwmMsg.dutyCycle = req.lightPwmDutyCycle;
    lightPwmMsg.ctrl_frame_1 = CTRL_WORD_1;

    serPort.write( (uint8_t*)(&lightPwmMsg), sizeof(PwmData) );

    res.settingOk = true;
    return true;
}

int main(int argc, char** argv)
{
    ros::init(argc, argv, "rs_sensor_board_node");

    ROS_INFO_STREAM("-----------------------------------\r");
    ROS_INFO_STREAM("    RoboSec Sensor Board driver     \r");
    ROS_INFO_STREAM("-----------------------------------\r");

    nh = new ros::NodeHandle(); // Node
    nhPriv = new ros::NodeHandle( "~" ); // Private node to load parameters

    /*if( ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Debug) )
    {
        ros::console::notifyLoggerLevelsChanged();
    }*/

    // Load parameters from param server
    loadParams();

    // >>>>> Light change service
    ros::ServiceServer lightSrv = nh->advertiseService( "camera_light", changeLight );
    // <<<<< Light change service

    // >>>>> Output message
    ultrasnd_bump_ranges rangeMsg;

    std_msgs::Header headerFL;
    headerFL.frame_id = "ultraSnd_FL";
    std_msgs::Header headerFR;
    headerFR.frame_id = "ultraSnd_FR";
    std_msgs::Header headerRR;
    headerRR.frame_id = "ultraSnd_RR";
    std_msgs::Header headerRL;
    headerRL.frame_id = "ultraSnd_RL";

    rangeMsg.sensor_FL.header = headerFL;
    rangeMsg.sensor_FR.header = headerFR;
    rangeMsg.sensor_RR.header = headerRR;
    rangeMsg.sensor_RL.header = headerRL;

    rangeMsg.sensor_FL.radiation_type = sensor_msgs::Range::ULTRASOUND;
    rangeMsg.sensor_FR.radiation_type = sensor_msgs::Range::ULTRASOUND;
    rangeMsg.sensor_RR.radiation_type = sensor_msgs::Range::ULTRASOUND;
    rangeMsg.sensor_RL.radiation_type = sensor_msgs::Range::ULTRASOUND;

    rangeMsg.sensor_FL.field_of_view = angles::from_degrees( 30.0f );
    rangeMsg.sensor_FR.field_of_view = angles::from_degrees( 30.0f );
    rangeMsg.sensor_RR.field_of_view = angles::from_degrees( 30.0f );
    rangeMsg.sensor_RL.field_of_view = angles::from_degrees( 30.0f );

    rangeMsg.sensor_FL.min_range = 0.0f;
    rangeMsg.sensor_FL.max_range = INVALID_REMAP;
    rangeMsg.sensor_FR.min_range = 0.0f;
    rangeMsg.sensor_FR.max_range = INVALID_REMAP;
    rangeMsg.sensor_RR.min_range = 0.0f;
    rangeMsg.sensor_RR.max_range = INVALID_REMAP;
    rangeMsg.sensor_RL.min_range = 0.0f;
    rangeMsg.sensor_RL.max_range = INVALID_REMAP;

    static ros::Publisher range_pub = nh->advertise<ultrasnd_bump_ranges>( "ranges", 10, false );
    // <<<<< Output message

    // >>>>> Serial driver
    bool connected = false;

    if( !simul )
        connected = connect_serial();

    string ser_buffer;

    UltraSndDataOut received;
    uint8_t ctrl0_0;
    uint8_t ctrl0_1;

    uint8_t connect_retry = 0;

    while( ros::ok() )
    {
        if( !simul && connected )
        {
            if( serPort.waitReadable() )
            {
                int available = serPort.available();
                ser_buffer += serPort.read( available );

                // >>>>> Searching for first byte: 0xA5
                bool found = false;
                for( int i=0; i<ser_buffer.size(); i++ )
                {
                    ctrl0_0 = ser_buffer.at(i);
                    if( ctrl0_0 != 0xa5 )
                        ser_buffer.erase( ser_buffer.begin(),
                                          ser_buffer.begin()+1 );
                    else
                    {
                        found = true;
                        break;
                    }
                }

                if(!found)
                {
                    ROS_DEBUG_STREAM( "Sync [0xa5] not found" );
                    continue;
                }
                // <<<<< Searching for first byte: 0xA5

                // >>>>> Data received is complete?
                if( ser_buffer.size()<sizeof(UltraSndDataOut) )
                {
                    ROS_DEBUG_STREAM( "Data incomplete" );
                    continue;
                }
                // <<<<< Data received is complete?

                // >>>>> Second byte is correct? [0x5A]
                ctrl0_1 = ser_buffer.at(1);
                if( ctrl0_1 != 0x5a )
                    continue;
                // <<<<< Second byte is correct? [0x5A]

                uint8_t type=0xff;

                type = ser_buffer.at(3);

                if( type==MSG_ULTRASOUND )
                {
                    // Data copy
                    memcpy( (char*)&received, ser_buffer.data(), sizeof(UltraSndDataOut) );

                    // >>>>> Terminator is correct? [0x0d0a]
                    if( received.ctrl_frame_1 != 0x0d0a )
                    {
                        ROS_DEBUG_STREAM( "Bad data!!!" );

                        // If the terminator is not correct I remove only the Synchronizing
                        // word [0x5AA5] because it was a false beginning.
                        // The next cycle I start searching for first byte [0xA5] again

                        ser_buffer.erase( ser_buffer.begin(),
                                          ser_buffer.begin()+2 );
                        continue;
                    }
                    // <<<<< Terminator is correct? [0x0d0a]

                    // Removing processed data
                    ser_buffer.erase( ser_buffer.begin(),
                                      ser_buffer.begin()+ sizeof(UltraSndDataOut) );
                }
                else
                {
                    ROS_WARN_STREAM( "Received invalide message type: " << type );
                }
            }
            else // Distance bypass to not block "twist messages"
            {
                ROS_WARN_STREAM( "Serial port " << serial_port_name << " timeout " );

                received.distances[0] = INVALID_REMAP;
                received.distances[1] = INVALID_REMAP;
                received.distances[2] = INVALID_REMAP;
                received.distances[3] = INVALID_REMAP;
            }
        }
        else // Data simulation && Not Connected bypass
        {
            if( simul )
            {
                received.distances[0] = ((float)std::rand()/RAND_MAX) * INVALID_REMAP;
                received.distances[1] = ((float)std::rand()/RAND_MAX) * INVALID_REMAP;
                received.distances[2] = ((float)std::rand()/RAND_MAX) * INVALID_REMAP;
                received.distances[3] = ((float)std::rand()/RAND_MAX) * INVALID_REMAP;
            }
            else
            {
                connect_retry++;

                if( connect_retry==20 )
                {
                    connected = connect_serial();
                    connect_retry = 0;
                }

                received.distances[0] = INVALID_REMAP;
                received.distances[1] = INVALID_REMAP;
                received.distances[2] = INVALID_REMAP;
                received.distances[3] = INVALID_REMAP;
            }
        }

        // >>>>> Output message
        ros::Time now = ros::Time::now();
        rangeMsg.sensor_FL.header.stamp = now;
        rangeMsg.sensor_FR.header.stamp = now;
        rangeMsg.sensor_RR.header.stamp = now;
        rangeMsg.sensor_RL.header.stamp = now;

        if( received.distances[0] != received.not_valid_val )
            rangeMsg.sensor_FL.range = received.distances[0];
        else
            rangeMsg.sensor_FL.range = INVALID_REMAP;
        if( received.distances[1] != received.not_valid_val )
            rangeMsg.sensor_FR.range = received.distances[1];
        else
            rangeMsg.sensor_FR.range = INVALID_REMAP;
        if( received.distances[2] != received.not_valid_val )
            rangeMsg.sensor_RR.range = received.distances[2];
        else
            rangeMsg.sensor_RR.range = INVALID_REMAP;
        if( received.distances[3] != received.not_valid_val )
            rangeMsg.sensor_RL.range = received.distances[3];
        else
            rangeMsg.sensor_RL.range = INVALID_REMAP;

        range_pub.publish( rangeMsg );
        // <<<<< Output message

        ros::spinOnce();

        ros::Rate r(10.0); // 10 hz
        r.sleep();
    }

    ROS_INFO_STREAM("... stopped!");

    return EXIT_SUCCESS;
}

bool connect_serial()
{
    try
    {
        serPort.setPort(serial_port_name);
        serPort.open();

        if(serPort.isOpen()){
            ROS_INFO_STREAM("Serial Port initialized: " << serial_port_name );
        }
        else
        {
            ROS_ERROR_STREAM( "Serial port not opened: " << serial_port_name );
            return EXIT_FAILURE;
        }

        serPort.setBaudrate(baudrate);

        serial::Timeout to = serial::Timeout::simpleTimeout(timeout_msec);
        serPort.setTimeout(to);
    }
    catch (serial::IOException& e)
    {
        ROS_ERROR_STREAM("Unable to configure serial port " << serial_port_name << " - Error: "  << e.what() );
        return false;
    }

    return true;
}

void loadParams()
{
    ROS_INFO_STREAM( "Loading parameters from server" );

    if( !nhPriv->getParam( "serial_port", serial_port_name ) )
    {
        serial_port_name = DEFAULT_SER_PORT;
        nhPriv->setParam( "serial_port", serial_port_name );
        ROS_INFO_STREAM( "serial_port" << " not present. Default value set: " << serial_port_name );
    }
    else
        ROS_DEBUG_STREAM( "serial_port: " << serial_port_name );

    if( !nhPriv->getParam( "baudrate", baudrate ) )
    {
        baudrate = DEFAULT_BAUDRATE;
        nhPriv->setParam( "baudrate", baudrate );
        ROS_INFO_STREAM( "baudrate: " << " not present. Default value set: " << baudrate );
    }
    else
        ROS_DEBUG_STREAM( "baudrate: " << baudrate );

    if( !nhPriv->getParam( "timeout_msec", timeout_msec ) )
    {
        timeout_msec = DEFAULT_TIMEOUT;
        nhPriv->setParam( "timeout_msec", timeout_msec );
        ROS_INFO_STREAM( "timeout_msec" << " not present. Default value set: " << timeout_msec );
    }
    else
        ROS_DEBUG_STREAM( "timeout_msec " << timeout_msec );

    if( !nhPriv->getParam( "simul", simul ) )
    {
        simul = false;
        nhPriv->setParam( "simul", simul );
        ROS_INFO_STREAM( "simul" << " not present. Default value set: " << simul );
    }
    else
        ROS_DEBUG_STREAM( "simul " << simul );
}
