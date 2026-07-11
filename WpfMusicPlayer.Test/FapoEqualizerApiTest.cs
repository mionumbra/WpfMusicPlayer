using MusicPlayerLibrary;

namespace WpfMusicPlayer.Test;

[TestClass]
public sealed class FapoEqualizerApiTest
{
    [TestMethod]
    public void ExistingTenBandApi_ClampsGain()
    {
        using var player = new MusicPlayerManaged();
        player.SetEqualizerBand(0, 100);
        Assert.AreEqual(24, player.GetEqualizerBand(0));
        player.SetEqualizerBand(0, -100);
        Assert.AreEqual(-24, player.GetEqualizerBand(0));
    }
}
