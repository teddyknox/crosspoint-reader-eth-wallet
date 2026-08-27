import CoreLocation
import Foundation
import WeatherKit

struct WeatherSnapshotResult {
    let data: Data
    let location: String
    let summary: String
    let generatedAt: Date
    let usedCachedData: Bool
}

enum WeatherProviderError: LocalizedError {
    case locationAccessRequired
    case locationUnavailable
    case weatherUnavailable

    var errorDescription: String? {
        switch self {
        case .locationAccessRequired: "Location access is required for weather"
        case .locationUnavailable: "Current location is unavailable"
        case .weatherUnavailable: "Weather data is unavailable"
        }
    }
}

@MainActor
final class WeatherSnapshotProvider: NSObject, CLLocationManagerDelegate {
    private let locationManager = CLLocationManager()
    private let weatherService = WeatherService.shared
    private let geocoder = CLGeocoder()
    private let defaults: UserDefaults
    private var locationContinuation: CheckedContinuation<CLLocation, Error>?

    private let snapshotKey = "cachedWeatherSnapshotV1"
    private let fingerprintKey = "cachedWeatherFingerprintV1"
    private let sequenceKey = "weatherSnapshotSequenceV1"
    private let locationKey = "cachedWeatherLocationV1"
    private let summaryKey = "cachedWeatherSummaryV1"
    private let generatedAtKey = "cachedWeatherGeneratedAtV1"

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        super.init()
        locationManager.delegate = self
        locationManager.desiredAccuracy = kCLLocationAccuracyKilometer
    }

    var authorizationStatus: CLAuthorizationStatus { locationManager.authorizationStatus }

    var hasLocationAccess: Bool {
        authorizationStatus == .authorizedAlways || authorizationStatus == .authorizedWhenInUse
    }

    var hasCachedSnapshot: Bool {
        defaults.data(forKey: snapshotKey)?.count == WeatherSnapshotEncoder.wireSize
    }

    var cachedLocation: String { defaults.string(forKey: locationKey) ?? "Current location" }

    var cachedSummary: String { defaults.string(forKey: summaryKey) ?? "Not refreshed" }

    func requestAccessAndRefresh() async throws -> WeatherSnapshotResult {
        try await refreshedSnapshot()
    }

    func refresh() async throws -> WeatherSnapshotResult {
        try await refreshedSnapshot()
    }

    func snapshot(refresh: Bool = true) async throws -> WeatherSnapshotResult {
        if let cached = cachedResult(),
           Date().timeIntervalSince(cached.generatedAt) < WeatherSnapshotEncoder.validityInterval {
            return cached
        }
        if refresh, hasLocationAccess {
            do {
                return try await refreshedSnapshot()
            } catch {
                if let cached = cachedResult() { return cached }
                throw error
            }
        }
        guard let cached = cachedResult() else {
            if !hasLocationAccess { throw WeatherProviderError.locationAccessRequired }
            throw WeatherProviderError.weatherUnavailable
        }
        return cached
    }

    func attribution() async throws -> WeatherAttribution {
        try await weatherService.attribution
    }

    private func refreshedSnapshot() async throws -> WeatherSnapshotResult {
        let location = try await currentLocation()
        let weather: Weather
        do {
            weather = try await weatherService.weather(for: location)
        } catch {
            throw WeatherProviderError.weatherUnavailable
        }
        let placeName = await reverseGeocodedName(for: location)
        let content = WeatherSnapshotContent(
            location: placeName,
            condition: readableCondition(weather.currentWeather.condition),
            temperature: temperature(weather.currentWeather.temperature),
            apparentTemperature: temperature(weather.currentWeather.apparentTemperature),
            humidity: percent(weather.currentWeather.humidity),
            wind: speed(weather.currentWeather.wind.speed),
            days: weather.dailyForecast.forecast.prefix(WeatherSnapshotEncoder.maxDays).map {
                WeatherDayItem(
                    date: $0.date,
                    condition: readableCondition($0.condition),
                    high: "H \(temperature($0.highTemperature, includeUnit: false))",
                    low: "L \(temperature($0.lowTemperature, includeUnit: false))",
                    precipitation: percent($0.precipitationChance)
                )
            }
        )
        let fingerprint = WeatherSnapshotEncoder.fingerprint(content)
        let sequence: UInt32
        let data: Data
        let previousGeneratedAt = Date(timeIntervalSince1970: defaults.double(forKey: generatedAtKey))
        let now = Date()
        if defaults.string(forKey: fingerprintKey) == fingerprint,
           now.timeIntervalSince(previousGeneratedAt) < WeatherSnapshotEncoder.validityInterval,
           let cached = defaults.data(forKey: snapshotKey), cached.count == WeatherSnapshotEncoder.wireSize {
            sequence = UInt32(defaults.integer(forKey: sequenceKey))
            data = cached
        } else {
            sequence = UInt32(truncatingIfNeeded: defaults.integer(forKey: sequenceKey) + 1)
            data = WeatherSnapshotEncoder.encode(content, sequence: sequence, now: now)
            defaults.set(data, forKey: snapshotKey)
            defaults.set(fingerprint, forKey: fingerprintKey)
            defaults.set(Int(sequence), forKey: sequenceKey)
            defaults.set(now.timeIntervalSince1970, forKey: generatedAtKey)
        }
        let summary = "\(content.temperature), \(content.condition)"
        defaults.set(placeName, forKey: locationKey)
        defaults.set(summary, forKey: summaryKey)
        let generatedAt = Date(timeIntervalSince1970: defaults.double(forKey: generatedAtKey))
        return WeatherSnapshotResult(data: data, location: placeName, summary: summary, generatedAt: generatedAt,
                                     usedCachedData: false)
    }

    private func cachedResult() -> WeatherSnapshotResult? {
        guard let data = defaults.data(forKey: snapshotKey), data.count == WeatherSnapshotEncoder.wireSize else {
            return nil
        }
        let generatedAt = Date(timeIntervalSince1970: defaults.double(forKey: generatedAtKey))
        return WeatherSnapshotResult(data: data, location: cachedLocation, summary: cachedSummary,
                                     generatedAt: generatedAt, usedCachedData: true)
    }

    private func currentLocation() async throws -> CLLocation {
        if locationContinuation != nil {
            throw WeatherProviderError.locationUnavailable
        }
        return try await withCheckedThrowingContinuation { continuation in
            locationContinuation = continuation
            switch authorizationStatus {
            case .authorizedAlways, .authorizedWhenInUse:
                locationManager.requestLocation()
            case .notDetermined:
                locationManager.requestWhenInUseAuthorization()
            case .denied, .restricted:
                locationContinuation = nil
                continuation.resume(throwing: WeatherProviderError.locationAccessRequired)
            @unknown default:
                locationContinuation = nil
                continuation.resume(throwing: WeatherProviderError.locationUnavailable)
            }
        }
    }

    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        guard locationContinuation != nil else { return }
        switch manager.authorizationStatus {
        case .authorizedAlways, .authorizedWhenInUse:
            manager.requestLocation()
        case .denied, .restricted:
            resumeLocation(throwing: WeatherProviderError.locationAccessRequired)
        case .notDetermined:
            break
        @unknown default:
            resumeLocation(throwing: WeatherProviderError.locationUnavailable)
        }
    }

    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let location = locations.last else {
            resumeLocation(throwing: WeatherProviderError.locationUnavailable)
            return
        }
        resumeLocation(returning: location)
    }

    func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        resumeLocation(throwing: error)
    }

    private func resumeLocation(returning location: CLLocation) {
        let continuation = locationContinuation
        locationContinuation = nil
        continuation?.resume(returning: location)
    }

    private func resumeLocation(throwing error: Error) {
        let continuation = locationContinuation
        locationContinuation = nil
        continuation?.resume(throwing: error)
    }

    private func reverseGeocodedName(for location: CLLocation) async -> String {
        guard let placemark = try? await geocoder.reverseGeocodeLocation(location).first else {
            return "Current location"
        }
        return placemark.locality ?? placemark.subAdministrativeArea ?? placemark.administrativeArea ?? "Current location"
    }

    private func temperature(_ measurement: Measurement<UnitTemperature>, includeUnit: Bool = true) -> String {
        let metric = Locale.autoupdatingCurrent.measurementSystem == .metric
        let converted = measurement.converted(to: metric ? .celsius : .fahrenheit)
        return "\(Int(converted.value.rounded()))°\(includeUnit ? (metric ? "C" : "F") : "")"
    }

    private func speed(_ measurement: Measurement<UnitSpeed>) -> String {
        let metric = Locale.autoupdatingCurrent.measurementSystem == .metric
        let converted = measurement.converted(to: metric ? .kilometersPerHour : .milesPerHour)
        return "\(Int(converted.value.rounded())) \(metric ? "km/h" : "mph")"
    }

    private func percent(_ value: Double) -> String { "\(Int((value * 100).rounded()))%" }

    private func readableCondition(_ condition: WeatherCondition) -> String {
        let raw = String(describing: condition)
        guard !raw.contains(" ") else { return raw.capitalized }
        let spaced = raw.replacingOccurrences(of: "([a-z])([A-Z])", with: "$1 $2", options: .regularExpression)
        return spaced.capitalized
    }
}
