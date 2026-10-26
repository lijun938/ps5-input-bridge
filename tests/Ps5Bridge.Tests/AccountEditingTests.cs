using Ps5Bridge.Core;
using Xunit;

namespace Ps5Bridge.Tests;

public sealed class AccountEditingTests
{
    [Fact]
    public void RenameAndAddressRepairKeepIdentityAndKeys()
    {
        var original = AccountStoreTests.Profile();
        var renamed = AccountEditing.Update(original, "  朋友的账户  ", original.Host);
        Assert.Equal(original with { Name = "朋友的账户" }, renamed);
        var moved = AccountEditing.Update(renamed, renamed.Name, "192.168.1.99");
        Assert.Equal(renamed with { Host = "192.168.1.99" }, moved);
        Assert.Throws<ArgumentException>(() => AccountEditing.Update(original, " ", original.Host));
        Assert.Throws<ArgumentException>(() => AccountEditing.Update(original, original.Name, "http://192.168.1.2"));
    }

    [Fact]
    public void EditingAndDeletingTargetIdDespiteMatchingNamesAndHosts()
    {
        var a = AccountStoreTests.Profile("同名");
        var b = a with { Id = Guid.NewGuid(), Keys = a.Keys with { RegistrationKeyHex = new string('c', 32) } };
        var edited = AccountEditing.Replace([a, b], b with { Name = "已改名", Keys = a.Keys });
        Assert.Equal(a, edited[0]);
        Assert.Equal(b with { Name = "已改名" }, edited[1]);
        Assert.Equal(new[] { a }, AccountEditing.Remove(edited, b.Id));
        Assert.Empty(AccountEditing.Remove([a], a.Id));
    }
}
