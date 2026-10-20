using System.Windows;

namespace Ps5Bridge.App;

/// <summary>Asks for the password of a portable account bundle.</summary>
public partial class BundlePasswordWindow : Window
{
    private readonly bool exporting;

    public string? Password { get; private set; }

    public BundlePasswordWindow(bool exporting)
    {
        InitializeComponent();
        this.exporting = exporting;
        HeadingText.Text = exporting ? "为账户文件设置密码" : "输入账户文件密码";
        HintText.Text = exporting
            ? "导出的文件会带走所有已保存账户的配对凭据，请设置至少 8 个字符的密码。文件复制到另一台电脑后，输入同一密码即可导入。"
            : "请输入导出账户文件时设置的密码。";
        SubmitButton.Content = exporting ? "导出" : "导入";
        Loaded += (_, _) => PasswordInput.Focus();
        Closed += (_, _) => PasswordInput.Clear();
    }

    private void Submit_Click(object sender, RoutedEventArgs e)
    {
        var password = PasswordInput.Password;
        if (password.Length < (exporting ? 8 : 1))
        {
            ErrorText.Visibility = Visibility.Visible;
            return;
        }
        Password = password;
        PasswordInput.Clear();
        DialogResult = true;
    }
}
