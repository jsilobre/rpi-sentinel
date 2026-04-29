#include <gtest/gtest.h>
#include "../src/sensors/CpuTempReader.hpp"

#include <filesystem>
#include <fstream>

// Writes content to a temp file named after the current test and returns its path.
static std::filesystem::path write_thermal_file(const std::string& content)
{
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = info ? info->name() : "unknown";
    auto path = std::filesystem::temp_directory_path() / ("rpi_cputemp_" + name);
    std::ofstream(path) << content;
    return path;
}

TEST(CpuTempReader, ReadsTypicalValue)
{
    auto path = write_thermal_file("48750\n");
    rpi::CpuTempReader reader("cpu", path);

    auto result = reader.read();
    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result->value, 48.75f, 0.001f);
    EXPECT_EQ(result->metric,    "temperature");
    EXPECT_EQ(result->sensor_id, "cpu");
}

TEST(CpuTempReader, ReadsValueWithoutNewline)
{
    auto path = write_thermal_file("55000");
    rpi::CpuTempReader reader("mycpu", path);

    auto result = reader.read();
    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result->value, 55.0f, 0.001f);
}

TEST(CpuTempReader, ReadsZeroDegrees)
{
    auto path = write_thermal_file("0\n");
    rpi::CpuTempReader reader("cpu", path);

    auto result = reader.read();
    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result->value, 0.0f, 0.001f);
}

TEST(CpuTempReader, SensorIdReturnsConfiguredId)
{
    rpi::CpuTempReader reader("rpi-cpu");
    EXPECT_EQ(reader.sensor_id(), "rpi-cpu");
}

TEST(CpuTempReader, DeviceNotFoundOnMissingFile)
{
    rpi::CpuTempReader reader("cpu", "/tmp/rpi_no_such_thermal_file_xyz");
    auto result = reader.read();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), rpi::SensorError::DeviceNotFound);
}

TEST(CpuTempReader, ParseErrorOnNonNumericContent)
{
    auto path = write_thermal_file("not_a_number\n");
    rpi::CpuTempReader reader("cpu", path);

    auto result = reader.read();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), rpi::SensorError::ParseError);
}

TEST(CpuTempReader, HandlesHighTemperature)
{
    auto path = write_thermal_file("85000\n");
    rpi::CpuTempReader reader("cpu", path);

    auto result = reader.read();
    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result->value, 85.0f, 0.001f);
}
