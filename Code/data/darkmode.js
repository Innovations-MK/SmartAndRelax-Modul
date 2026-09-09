                                                            

                                                          
var darkModeToggle = document.getElementById("darkModeToggle");

if (localStorage.getItem("darkModeStatus")) {
                                                       
  darkModeToggle.checked = localStorage.getItem("darkModeStatus") === "On";

                                                     
  toggleDarkMode();
}

function toggleDarkMode() {
  var sliderElement = darkModeForm.querySelector(".slider");
  if (darkModeToggle.checked) {
                      
    document.documentElement.classList.add("darkmode");
    localStorage.setItem("darkModeStatus", "On");
    sliderElement.classList.remove("moon");
    sliderElement.classList.add("sun");
  } else {
                       
    document.documentElement.classList.remove("darkmode");
    localStorage.setItem("darkModeStatus", "Off");
    sliderElement.classList.remove("sun");
    sliderElement.classList.add("moon");
  }
}

                                       
document.addEventListener("DOMContentLoaded", function () {
  const topNavIcon = document.querySelector(".topnavicon");

  topNavIcon.addEventListener("click", function () {
    topNavIcon.classList.toggle("show-before");
    const afterIcon = topNavIcon.nextElementSibling;
    afterIcon.style.display = afterIcon.style.display === "none" ? "inline" : "none";
  });
});
