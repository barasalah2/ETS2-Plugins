const { truckSimTelemetry } = require('trucksim-telemetry');

const telemetry = truckSimTelemetry();

telemetry.on('connected', () => {
  console.log('Telemetry connected!');
});

telemetry.on('data', data => {
  console.log('Telemetry data:', JSON.stringify(data, null, 2));
  process.exit(0); // Exit after first data event
});

telemetry.on('error', err => {
  console.error('Telemetry error:', err);
  process.exit(1);
});