namespace Ps5Bridge.Core;

/// <summary>Changes display details without replacing a paired account's identity or secrets.</summary>
public static class AccountEditing
{
    public static AccountProfile Update(AccountProfile original, string name, string host)
    {
        var updated = original with { Name = name.Trim(), Host = host.Trim() };
        AccountStore.Validate(updated);
        return updated;
    }

    public static AccountProfile[] Replace(IEnumerable<AccountProfile> profiles, AccountProfile replacement)
    {
        var result = profiles.ToArray();
        var index = Array.FindIndex(result, p => p.Id == replacement.Id);
        if (index < 0) throw new InvalidOperationException("账户已不存在，请重新选择。");
        result[index] = Update(result[index], replacement.Name, replacement.Host);
        return result;
    }

    public static AccountProfile[] Remove(IEnumerable<AccountProfile> profiles, Guid id) =>
        profiles.Where(p => p.Id != id).ToArray();
}
