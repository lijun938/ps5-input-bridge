using System.Windows;
using Ps5Bridge.Core;

namespace Ps5Bridge.App;

public partial class EditAccountWindow : Window
{
    private readonly AccountProfile original;
    public AccountProfile? Profile { get; private set; }

    public EditAccountWindow(AccountProfile profile)
    {
        InitializeComponent();
        original = profile;
        NameInput.Text = profile.Name;
        HostInput.Text = profile.Host;
    }

    private void Save_Click(object sender, RoutedEventArgs e)
    {
        try { Profile = AccountEditing.Update(original, NameInput.Text, HostInput.Text); DialogResult = true; }
        catch (ArgumentException ex) { ErrorText.Text = ex.Message; ErrorText.Visibility = Visibility.Visible; }
    }
}
