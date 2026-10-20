using System.Windows;

namespace Ps5Bridge.App;

public partial class LoginPinWindow : Window
{
    public string? Pin { get; private set; }

    public LoginPinWindow()
    {
        InitializeComponent();
        Loaded += (_, _) => PinInput.Focus();
        Closed += (_, _) => PinInput.Clear();
    }

    private void Submit_Click(object sender, RoutedEventArgs e)
    {
        var pin = PinInput.Password;
        if (pin.Length != 4 || pin.Any(character => character is < '0' or > '9'))
        {
            ErrorText.Visibility = Visibility.Visible;
            return;
        }
        Pin = pin;
        PinInput.Clear();
        DialogResult = true;
    }

    public void Cancel()
    {
        if (IsVisible) DialogResult = false;
    }
}
