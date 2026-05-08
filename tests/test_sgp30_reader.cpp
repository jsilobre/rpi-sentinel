#include <gtest/gtest.h>
#include "../src/sensors/SGP30Reader.hpp"

#include <filesystem>
#include <fstream>

static std::filesystem::path make_iio_dir(const std::string& test_name)
{
    auto dir = std::filesystem::temp_directory_path() / ("rpi_sgp30_" + test_name);
    std::filesystem::create_directories(dir);
    return dir;
}

static void write_file(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream(path) << content;
}

TEST(SGP30Reader, ReadsEco2)
{
    auto dir = make_iio_dir("ReadsEco2");
    write_file(dir / "in_concentration_co2_input", "823\n");

    rpi::SGP30Reader reader(dir.string(), "sgp30-eco2", "eco2");
    auto result = reader.read();

    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result->value, 823.0f, 0.001f);
    EXPECT_EQ(result->metric,    "eco2");
    EXPECT_EQ(result->sensor_id, "sgp30-eco2");
}

TEST(SGP30Reader, ReadsTvoc)
{
    auto dir = make_iio_dir("ReadsTvoc");
    write_file(dir / "in_concentration_voc_input", "47\n");

    rpi::SGP30Reader reader(dir.string(), "sgp30-tvoc", "tvoc");
    auto result = reader.read();

    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result->value, 47.0f, 0.001f);
    EXPECT_EQ(result->metric,    "tvoc");
    EXPECT_EQ(result->sensor_id, "sgp30-tvoc");
}

TEST(SGP30Reader, ReadsZeroEco2)
{
    auto dir = make_iio_dir("ReadsZeroEco2");
    write_file(dir / "in_concentration_co2_input", "400\n");

    rpi::SGP30Reader reader(dir.string(), "sgp30-eco2", "eco2");
    auto result = reader.read();

    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result->value, 400.0f, 0.001f);
}

TEST(SGP30Reader, SensorIdReturnsConfiguredId)
{
    rpi::SGP30Reader reader("/nonexistent", "my-sgp30", "eco2");
    EXPECT_EQ(reader.sensor_id(), "my-sgp30");
}

TEST(SGP30Reader, DeviceNotFoundOnMissingDirectory)
{
    rpi::SGP30Reader reader("/tmp/rpi_no_such_iio_device_xyz", "sgp30-eco2", "eco2");
    auto result = reader.read();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), rpi::SensorError::DeviceNotFound);
}

TEST(SGP30Reader, ParseErrorOnUnknownMetric)
{
    auto dir = make_iio_dir("ParseErrorUnknownMetric");
    rpi::SGP30Reader reader(dir.string(), "sgp30", "pressure");
    auto result = reader.read();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), rpi::SensorError::ParseError);
}

TEST(SGP30Reader, ReadFailureOnNonNumericContent)
{
    auto dir = make_iio_dir("ReadFailureNonNumeric");
    write_file(dir / "in_concentration_co2_input", "not_a_number\n");

    rpi::SGP30Reader reader(dir.string(), "sgp30-eco2", "eco2");
    auto result = reader.read();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), rpi::SensorError::ReadFailure);
}

TEST(SGP30Reader, ReadsHighCo2Value)
{
    auto dir = make_iio_dir("ReadsHighCo2Value");
    write_file(dir / "in_concentration_co2_input", "5000\n");

    rpi::SGP30Reader reader(dir.string(), "sgp30-eco2", "eco2");
    auto result = reader.read();

    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result->value, 5000.0f, 0.001f);
}
