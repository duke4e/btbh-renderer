import BTBHRendererCore

public enum RendererABI {
    public static let version: UInt32 = 1
    public static var cVersion: UInt32 { btbh_renderer_abi_version() }
}
